#include "ota_manager.h"
#include "nvs_config.h"
#include "wifi_manager.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Przeniesione z SIHOS17 (ota_manager.c, beta410/411) - ta sama logika
// pobierania z wznawianiem, kanaly oficjalny/beta i sprawdzanie wersji.

static const char *TAG = "OTA";
static ota_status_t  s_status = { .state = OTA_STATE_IDLE };
static volatile bool s_upload_busy = false;

#define GH_REPO        "smartinhome/sihradar"
#define GITHUB_API_URL "https://api.github.com/repos/" GH_REPO "/releases?per_page=15"
#define GITHUB_DL_BASE "https://github.com/" GH_REPO "/releases/"
#define GITHUB_BIN     "sih-radar.bin"
#define PROJECT_NAME   "sih-radar"      // project() w firmware/CMakeLists.txt
#define USER_AGENT     "SIH-Radar"

// Whitelist zrodel: bez niej ktos w sieci lokalnej moglby wskazac modulowi
// obraz z wlasnego serwera. Tylko https:// (podmiana w locie = obcy firmware).
// release-assets/objects.githubusercontent.com to CDN GitHuba po przekierowaniu.
static const char * const OTA_URL_WHITELIST[] = {
    "https://github.com/",
    "https://objects.githubusercontent.com/",
    "https://release-assets.githubusercontent.com/",
    "https://api.github.com/",
    "https://www.smartinhome.pl/",
    NULL
};

bool ota_url_allowed(const char *url) {
    if (!url || url[0] == 0) return false;
    for (int i = 0; OTA_URL_WHITELIST[i] != NULL; i++) {
        size_t plen = strlen(OTA_URL_WHITELIST[i]);
        if (strncmp(url, OTA_URL_WHITELIST[i], plen) == 0) return true;
    }
    return false;
}

bool ota_busy(void) {
    return s_upload_busy || s_status.state == OTA_STATE_DOWNLOADING ||
           s_status.state == OTA_STATE_WRITING;
}

void ota_set_upload_busy(bool on) { s_upload_busy = on; }

size_t ota_image_header_len(void) {
    return sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
}

// SIHOS17 tez dziala na ESP32-C6, wiec sam identyfikator ukladu nie wystarczy:
// sprawdzamy jeszcze nazwe projektu zaszyta w opisie obrazu.
const char *ota_check_image_header(const uint8_t *buf, size_t len) {
    if (len < ota_image_header_len()) return "Plik jest za krótki";
    const esp_image_header_t *ih = (const esp_image_header_t *)buf;
    const esp_app_desc_t *nd = (const esp_app_desc_t *)
        (buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (ih->magic != ESP_IMAGE_HEADER_MAGIC || nd->magic_word != ESP_APP_DESC_MAGIC_WORD)
        return "To nie jest plik firmware";
    if (ih->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID)
        return "Firmware dla innego układu ESP32";
    if (strncmp(nd->project_name, PROJECT_NAME, sizeof(nd->project_name)) != 0)
        return "To firmware innego urządzenia (nie SIH Radar)";
    return NULL;
}

// Kanal GitHub nie instaluje obrazu zbudowanego wczesniej niz zainstalowany.
static bool s_no_downgrade;
static bool s_gh_beta;
static char s_gh_tag[48];

// "Oct 27 2026" + "11:29:00" -> liczba rosnaca z czasem (0 = nie rozpoznano)
static int64_t ota_build_stamp(const char *date, const char *time) {
    static const char M[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int d = 0, y = 0, hh = 0, mm = 0, ss = 0;
    if (sscanf(date, "%3s %d %d", mon, &d, &y) != 3) return 0;
    const char *p = strstr(M, mon);
    if (strlen(mon) != 3 || !p || (p - M) % 3) return 0;
    sscanf(time, "%d:%d:%d", &hh, &mm, &ss);
    int mi = (int)(p - M) / 3 + 1;
    return ((((int64_t)y * 100 + mi) * 100 + d) * 100 + hh) * 10000 + mm * 100 + ss;
}

// ── Pobieranie obrazu z wznawianiem ──
// Jedno ciagle pobieranie, a po zerwaniu ponowne polaczenie od oryginalnego
// URL (swiezy podpisany link GitHuba) z naglowkiem Range od ostatniego
// zapisanego bajtu. Poprawnosc i tak sprawdza esp_ota_end (SHA-256 obrazu).
#define OTA_DL_BUF       4096
#define OTA_MAX_NOPROG   6     // kolejne proby bez zadnego postepu
#define OTA_MAX_TRIES    30    // twardy limit polaczen na jedna aktualizacje
#define OTA_MAX_STALL    2     // ile razy z rzedu read moze zwrocic timeout

static esp_http_client_handle_t ota_dl_open(const char *url, int64_t from,
                                            int *status, int64_t *clen) {
    *status = -1;
    *clen   = -1;
    esp_http_client_config_t cfg = {
        .url                         = url,
        .timeout_ms                  = 20000,
        .crt_bundle_attach           = esp_crt_bundle_attach,
        .skip_cert_common_name_check = false,
        .buffer_size                 = 8192,
        .buffer_size_tx              = 4096,
        .user_agent                  = USER_AGENT,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    if (from > 0) {
        char rng[32];
        snprintf(rng, sizeof(rng), "bytes=%lld-", (long long)from);
        esp_http_client_set_header(c, "Range", rng);   // zostaje po przekierowaniu
    }
    for (int hop = 0; hop < 8; hop++) {
        esp_err_t e = esp_http_client_open(c, 0);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "Polaczenie nieudane: %s", esp_err_to_name(e));
            break;
        }
        int64_t cl = esp_http_client_fetch_headers(c);
        int st = esp_http_client_get_status_code(c);
        if (cl < 0 || st <= 0) {
            ESP_LOGW(TAG, "Brak naglowkow odpowiedzi");
            break;
        }
        if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) {
            char tmp[128];   // oproznij tresc przekierowania przed kolejnym zadaniem
            while (esp_http_client_read(c, tmp, sizeof(tmp)) > 0) {}
            if (esp_http_client_set_redirection(c) != ESP_OK) break;
            esp_http_client_close(c);
            continue;
        }
        *status = st;
        *clen   = esp_http_client_get_content_length(c);
        return c;
    }
    esp_http_client_cleanup(c);
    return NULL;
}

typedef enum { DL_OK, DL_UPTODATE, DL_FAIL } dl_result_t;

static dl_result_t ota_download(const char *url) {
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        snprintf(s_status.error, sizeof(s_status.error), "Brak partycji OTA");
        return DL_FAIL;
    }
    char *buf = malloc(OTA_DL_BUF);
    if (!buf) {
        snprintf(s_status.error, sizeof(s_status.error), "Brak pamięci na bufor");
        return DL_FAIL;
    }

    esp_ota_handle_t oh = 0;
    bool    ota_open = false, complete = false, fatal = false;
    int64_t total = -1, done = 0;     // done = bajty juz zapisane na flash
    int     noprog = 0, tries = 0, last_decile = -1;
    const int hdr_need = (int)ota_image_header_len();
    s_status.error[0] = 0;

    while (!complete && !fatal && tries < OTA_MAX_TRIES && noprog < OTA_MAX_NOPROG) {
        if (tries > 0) {
            int wait_s = 2 * noprog + 1;
            if (wait_s > 10) wait_s = 10;
            ESP_LOGW(TAG, "Wznawiam od %lld B za %d s (proba %d)", (long long)done, wait_s, tries + 1);
            vTaskDelay(pdMS_TO_TICKS(wait_s * 1000));
        }
        tries++;
        int64_t done_before = done;
        int st; int64_t cl;
        esp_http_client_handle_t c = ota_dl_open(url, done, &st, &cl);
        if (!c) { noprog++; continue; }

        if (st == 404) {
            // latest/download daje 404, gdy nie ma zadnego wydania bez bety
            if (s_no_downgrade && !s_gh_beta)
                snprintf(s_status.error, sizeof(s_status.error), "Brak oficjalnego wydania na GitHub");
            else
                snprintf(s_status.error, sizeof(s_status.error), "Brak pliku firmware na GitHub (404)");
            fatal = true;
        } else if (st != 200 && st != 206) {
            ESP_LOGW(TAG, "HTTP %d", st);
            snprintf(s_status.error, sizeof(s_status.error), "Serwer odpowiedział HTTP %d", st);
        } else {
            // 200 przy wznowieniu = serwer zignorowal Range: pomin to, co juz jest
            int64_t skip = (st == 200) ? done : 0;
            if (cl > 0) {
                int64_t t = (st == 206) ? done + cl : cl;
                if (total < 0) total = t;
                else if (t != total) {
                    snprintf(s_status.error, sizeof(s_status.error), "Plik zmienił się w trakcie pobierania");
                    fatal = true;
                }
            }
            if (!fatal && total > (int64_t)part->size) {
                snprintf(s_status.error, sizeof(s_status.error), "Obraz większy od partycji OTA");
                fatal = true;
            }
            int fill = 0, stall = 0;
            while (!fatal) {
                int r = esp_http_client_read(c, buf + fill, OTA_DL_BUF - fill);
                if (r == -ESP_ERR_HTTP_EAGAIN) {
                    if (++stall > OTA_MAX_STALL) break;   // lacze stoi - polacz od nowa
                    continue;
                }
                if (r < 0) break;                          // zerwane
                if (r == 0) {
                    if (esp_http_client_is_complete_data_received(c) && ota_open)
                        complete = true;
                    break;
                }
                stall = 0;
                if (skip > 0) {
                    int k = (r > skip) ? (int)skip : r;
                    skip -= k;
                    if (k < r) memmove(buf + fill, buf + fill + k, r - k);
                    r -= k;
                    if (r == 0) continue;
                }
                fill += r;

                if (!ota_open) {
                    if (fill < hdr_need) continue;         // naglowek jeszcze niepelny
                    const char *bad = ota_check_image_header((const uint8_t *)buf, fill);
                    if (bad) {
                        strlcpy(s_status.error, bad, sizeof(s_status.error));
                        fatal = true;
                        break;
                    }
                    const esp_app_desc_t *nd = (const esp_app_desc_t *)
                        (buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
                    char ver[sizeof(nd->version) + 1];
                    memcpy(ver, nd->version, sizeof(nd->version));
                    ver[sizeof(nd->version)] = 0;
                    ESP_LOGI(TAG, "Aktualizacja do wersji %s (build %.16s %.16s)", ver, nd->date, nd->time);
                    const esp_app_desc_t *cur = esp_app_get_description();
                    if (strncmp(cur->version, nd->version, sizeof(nd->version)) == 0) {
                        snprintf(s_status.error, sizeof(s_status.error), "Wersja %s jest aktualna", ver);
                        esp_http_client_cleanup(c);
                        free(buf);
                        return DL_UPTODATE;
                    }
                    if (s_no_downgrade) {
                        int64_t sn = ota_build_stamp(nd->date, nd->time);
                        int64_t sc = ota_build_stamp(cur->date, cur->time);
                        if (sn && sc && sn < sc) {
                            snprintf(s_status.error, sizeof(s_status.error),
                                     "Zainstalowana jest nowsza niż %s", ver);
                            esp_http_client_cleanup(c);
                            free(buf);
                            return DL_UPTODATE;
                        }
                    }
                    ESP_LOGI(TAG, "Zapis do %s @0x%lx, rozmiar %lld B", part->label,
                             (unsigned long)part->address, (long long)total);
                    esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &oh);
                    if (e != ESP_OK) {
                        snprintf(s_status.error, sizeof(s_status.error), "esp_ota_begin: %s", esp_err_to_name(e));
                        fatal = true;
                        break;
                    }
                    ota_open = true;
                    s_status.state = OTA_STATE_WRITING;
                }

                esp_err_t e = esp_ota_write(oh, buf, fill);
                if (e != ESP_OK) {
                    snprintf(s_status.error, sizeof(s_status.error), "Zapis flash: %s", esp_err_to_name(e));
                    fatal = true;
                    break;
                }
                done += fill;
                fill = 0;
                if (total > 0) {
                    s_status.progress_pct = (int)(done * 100 / total);
                    if (s_status.progress_pct / 10 != last_decile) {
                        last_decile = s_status.progress_pct / 10;
                        ESP_LOGI(TAG, "Postep %d%% (%lld/%lld B)", s_status.progress_pct,
                                 (long long)done, (long long)total);
                    }
                    if (done >= total) { complete = true; break; }
                }
            }
        }
        esp_http_client_cleanup(c);
        if (done > done_before) noprog = 0;
        else if (!complete) noprog++;
    }
    free(buf);

    if (!complete) {
        if (ota_open) esp_ota_abort(oh);
        if (!fatal && (noprog >= OTA_MAX_NOPROG || tries >= OTA_MAX_TRIES)) {
            if (total > 0)
                snprintf(s_status.error, sizeof(s_status.error), "Pobieranie przerwane przy %d%% - słabe łącze?",
                         s_status.progress_pct);
            else if (!s_status.error[0])
                snprintf(s_status.error, sizeof(s_status.error), "Brak połączenia z serwerem firmware");
        }
        ESP_LOGE(TAG, "Niepowodzenie po %d polaczeniach: %s", tries, s_status.error);
        return DL_FAIL;
    }
    ESP_LOGI(TAG, "Pobrano %lld B w %d polaczeniach, sprawdzam obraz", (long long)done, tries);
    esp_err_t e = esp_ota_end(oh);
    if (e != ESP_OK) {
        snprintf(s_status.error, sizeof(s_status.error), "Obraz niepoprawny: %s", esp_err_to_name(e));
        return DL_FAIL;
    }
    e = esp_ota_set_boot_partition(part);
    if (e != ESP_OK) {
        snprintf(s_status.error, sizeof(s_status.error), "Ustawienie partycji: %s", esp_err_to_name(e));
        return DL_FAIL;
    }
    return DL_OK;
}

static void ota_url_task(void *arg) {
    char *url = (char *)arg;
    s_status.state        = OTA_STATE_DOWNLOADING;
    s_status.progress_pct = 0;

    // Ponowna walidacja: URL moze tez przyjsc z odpowiedzi API GitHuba
    if (!ota_url_allowed(url)) {
        ESP_LOGE(TAG, "URL '%s' spoza listy dozwolonych", url);
        snprintf(s_status.error, sizeof(s_status.error), "Adres spoza listy dozwolonych");
        s_status.state = OTA_STATE_FAILED;
        free(url);
        vTaskDelete(NULL);
        return;
    }

    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    ESP_LOGI(TAG, "Wolna pamiec %u B, najwiekszy blok %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "Pobieranie: %s", url);

    dl_result_t r = ota_download(url);
    free(url);
    if (r == DL_OK) {
        s_status.state        = OTA_STATE_SUCCESS;
        s_status.progress_pct = 100;
        ESP_LOGI(TAG, "Aktualizacja zapisana - restart za 3 s");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }
    s_status.state = (r == DL_UPTODATE) ? OTA_STATE_UPTODATE : OTA_STATE_FAILED;
    vTaskDelete(NULL);
}

// ── Adres najnowszej bety z API GitHuba (gdy panel nie podal tagu) ──
// Odpowiedz parsowana w locie przez przesuwane okno - bez trzymania calego
// JSON-a (ponad 100 KB) w RAM. Kanal oficjalny idzie przez latest/download.

typedef enum { GH_OK, GH_NOT_FOUND, GH_RATE_LIMIT, GH_NO_CONN, GH_HTTP, GH_NO_MEM } gh_result_t;

typedef struct {
    char best_tag[48];
    int  count;
    bool found;
} gh_pick_t;

// ".../releases/download/<tag>/sih-radar.bin" -> wybieramy najwyzszy tag bety
// (pelne porownanie wersji: v0.1.1-beta1 jest nowsza od v0.1.0-beta9)
static void github_consider_asset(const char *url, char *out_url, size_t out_max, gh_pick_t *st) {
    st->count++;
    const char *d = strstr(url, "/releases/download/");
    if (!d || !strstr(url, "/" GITHUB_BIN)) return;
    if (strlen(url) >= out_max) return;
    d += strlen("/releases/download/");
    const char *e = strchr(d, '/');
    if (!e || e - d <= 0 || (size_t)(e - d) >= sizeof(st->best_tag)) return;
    char tag[48];
    memcpy(tag, d, e - d);
    tag[e - d] = 0;
    if (!strstr(tag, "beta")) return;
    if (!st->found || ota_version_cmp(tag, st->best_tag) > 0) {
        strlcpy(st->best_tag, tag, sizeof(st->best_tag));
        strlcpy(out_url, url, out_max);
        st->found = true;
    }
}

static gh_result_t github_get_latest_beta_url(char *out_url, size_t out_max) {
    const size_t CAP  = 4096;
    const int    KEEP = 560;
    char *buf = malloc(CAP);
    if (!buf) return GH_NO_MEM;

    esp_http_client_config_t cfg = {
        .url                         = GITHUB_API_URL,
        .timeout_ms                  = 10000,
        .crt_bundle_attach           = esp_crt_bundle_attach,
        .skip_cert_common_name_check = false,
        .max_redirection_count       = 10,
        .buffer_size                 = 4096,
        .buffer_size_tx              = 1024,
        .user_agent                  = USER_AGENT,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(buf); return GH_NO_MEM; }
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        err = esp_http_client_open(client, 0);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "GitHub: polaczenie nieudane (%d/3)", attempt);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        free(buf);
        return GH_NO_CONN;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "GitHub: HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(buf);
        return (status == 403 || status == 429) ? GH_RATE_LIMIT : GH_HTTP;
    }

    const char *key = "\"browser_download_url\":\"";
    gh_pick_t st = { .count = 0, .found = false };
    int fill = 0, total = 0, r;
    while ((r = esp_http_client_read(client, buf + fill, (int)(CAP - 1 - fill))) > 0) {
        fill  += r;
        total += r;
        buf[fill] = 0;
        char *p = buf, *done = buf;
        while ((p = strstr(p, key)) != NULL) {
            char *v   = p + strlen(key);
            char *end = strchr(v, '"');
            if (!end) break;               // rekord uciety - dokonczymy po doczytaniu
            size_t ulen = end - v;
            if (ulen > 0 && ulen < 470) {
                char tmp[480];
                memcpy(tmp, v, ulen);
                tmp[ulen] = 0;
                github_consider_asset(tmp, out_url, out_max, &st);
            }
            p    = end + 1;
            done = p;
        }
        int keep = fill - (int)(done - buf);
        if (keep > KEEP) { done = buf + fill - KEEP; keep = KEEP; }
        if (keep < 0) keep = 0;
        if (keep > 0 && done != buf) memmove(buf, done, keep);
        fill = keep;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buf);
    if (total <= 0) return GH_HTTP;
    if (st.found) ESP_LOGI(TAG, "GitHub: najnowsza beta %s", st.best_tag);
    return st.found ? GH_OK : GH_NOT_FOUND;
}

static void ota_github_task(void *arg) {
    (void)arg;
    s_status.state        = OTA_STATE_DOWNLOADING;
    s_status.progress_pct = 0;
    s_status.error[0]     = 0;

    char url[480] = {0};
    if (!s_gh_beta) {
        // GitHub sam przekierowuje na plik z najnowszego wydania nie-beta
        // (bety maja flage prerelease) - bez API i jego limitu zapytan.
        strlcpy(url, GITHUB_DL_BASE "latest/download/" GITHUB_BIN, sizeof(url));
    } else if (s_gh_tag[0]) {
        snprintf(url, sizeof(url), GITHUB_DL_BASE "download/%s/" GITHUB_BIN, s_gh_tag);
    } else {
        gh_result_t g = github_get_latest_beta_url(url, sizeof(url));
        if (g != GH_OK) {
            const char *m =
                g == GH_RATE_LIMIT ? "GitHub: limit zapytań, spróbuj za godzinę" :
                g == GH_NO_CONN    ? "Brak połączenia z GitHub (internet/DNS)" :
                g == GH_HTTP       ? "GitHub odpowiedział błędem, spróbuj później" :
                g == GH_NO_MEM     ? "Za mało pamięci na połączenie z GitHub" :
                                     "Nie znaleziono wersji beta na GitHub";
            strlcpy(s_status.error, m, sizeof(s_status.error));
            s_status.state = OTA_STATE_FAILED;
            vTaskDelete(NULL);
            return;
        }
    }
    ESP_LOGI(TAG, "Kanal %s -> %s", s_gh_beta ? "beta" : "oficjalny", url);
    s_no_downgrade = true;
    ota_url_task(strdup(url));   // wykona sie w tym samym tasku
}

// Tag z panelu trafia do URL - tylko bezpieczne znaki nazwy tagu
static bool gh_tag_ok(const char *t) {
    size_t n = strlen(t);
    if (n == 0 || n >= sizeof(s_gh_tag)) return false;
    for (size_t i = 0; i < n; i++) {
        char ch = t[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_'))
            return false;
    }
    return strstr(t, "beta") != NULL;   // kanaly sie nie mieszaja
}

void ota_start_from_github(bool beta_channel, const char *tag) {
    if (ota_busy()) {
        ESP_LOGW(TAG, "Aktualizacja juz trwa");
        return;
    }
    s_status.state    = OTA_STATE_DOWNLOADING;   // od razu: panel nie zobaczy starego stanu
    s_status.progress_pct = 0;
    s_status.error[0] = 0;
    s_gh_beta = beta_channel;
    s_gh_tag[0] = 0;
    if (beta_channel && tag && tag[0]) {
        if (gh_tag_ok(tag)) strlcpy(s_gh_tag, tag, sizeof(s_gh_tag));
        else ESP_LOGW(TAG, "Odrzucony tag '%s' - szukam przez API", tag);
    }
    if (xTaskCreate(ota_github_task, "ota_github", 16384, NULL, 6, NULL) != pdPASS) {
        strlcpy(s_status.error, "Za mało pamięci na zadanie aktualizacji", sizeof(s_status.error));
        s_status.state = OTA_STATE_FAILED;
    }
}

void ota_start_from_url(const char *url) {
    if (ota_busy()) {
        ESP_LOGW(TAG, "Aktualizacja juz trwa");
        return;
    }
    char *url_copy = strdup(url);
    if (!url_copy) return;
    s_status.state = OTA_STATE_DOWNLOADING;
    s_status.progress_pct = 0;
    s_status.error[0] = 0;
    s_no_downgrade = false;   // wlasny URL: uzytkownik wie, co wgrywa
    s_gh_beta = false;
    if (xTaskCreate(ota_url_task, "ota_url", 16384, url_copy, 5, NULL) != pdPASS) {
        free(url_copy);
        strlcpy(s_status.error, "Za mało pamięci na zadanie aktualizacji", sizeof(s_status.error));
        s_status.state = OTA_STATE_FAILED;
    }
}

// ── Sprawdzanie nowej wersji oficjalnej ──
// Modul pyta github.com o /releases/latest: 5 min po starcie, potem co 12 h
// i na przycisk w panelu. GitHub przekierowuje na .../releases/tag/vX.Y.Z
// najnowszego wydania bez flagi pre-release, a gdy go nie ma - na liste
// .../releases. To zwykla strona, nie API, wiec nie zuzywa limitu zapytan.
// Niczego nie instaluje: panel pokazuje plakietke.
#define UPD_URL       "https://github.com/" GH_REPO "/releases/latest"
#define UPD_FIRST_S   (5 * 60)
#define UPD_PERIOD_S  (12 * 60 * 60)
#define UPD_RETRY_S   (30 * 60)

typedef enum { UPD_UNKNOWN, UPD_OK, UPD_NONE } upd_state_t;
static upd_state_t        s_upd_state;
static bool               s_upd_err;
static char               s_upd_latest[32];
static volatile bool      s_upd_busy;
static esp_timer_handle_t s_upd_timer;

// "v1.0.1" -> {1,0,1,INT_MAX}, "v1.0.1-beta3" -> {1,0,1,3}: wydanie oficjalne
// jest nowsze od kazdej swojej bety (v1.0.0-beta9 < v1.0.0 < v1.0.1-beta1).
static bool ver_parse(const char *s, int v[4]) {
    if (!s) return false;
    if (*s == 'v' || *s == 'V') s++;
    if (sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) != 3) return false;
    const char *bp = strstr(s, "-beta");
    v[3] = bp ? atoi(bp + 5) : INT_MAX;
    return true;
}

int ota_version_cmp(const char *a, const char *b) {
    int va[4], vb[4];
    bool oa = ver_parse(a, va), ob = ver_parse(b, vb);
    if (!oa || !ob) return (int)oa - (int)ob;
    for (int i = 0; i < 4; i++)
        if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
    return 0;
}

// Adres po przekierowaniu -> tag. 1 = wydanie oficjalne (tag w out),
// 0 = brak wydania oficjalnego, -1 = nierozpoznany adres.
static int upd_tag_from_url(const char *loc, char *out, size_t cap) {
    const char *t = strstr(loc, "/releases/tag/");
    if (t) {
        t += strlen("/releases/tag/");
        size_t n = 0;
        while (t[n] && t[n] != '/' && t[n] != '?' && t[n] != '#') {
            char ch = t[n];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_'))
                return -1;
            n++;
        }
        if (n == 0 || n >= cap) return -1;
        memcpy(out, t, n);
        out[n] = 0;
        return strstr(out, "beta") ? -1 : 1;   // beta nie moze byc "latest"
    }
    size_t l = strlen(loc);
    if (l >= 9 && strcmp(loc + l - 9, "/releases") == 0) return 0;
    return -1;
}

static void upd_schedule(int sec) {
    if (!s_upd_timer) return;
    esp_timer_stop(s_upd_timer);
    esp_timer_start_once(s_upd_timer, (uint64_t)sec * 1000000ULL);
}

static bool upd_fetch(void) {
    // TLS do GitHuba potrzebuje spojnego bloku sterty - bez niego nie probujemy
    size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t fr  = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (big < 24 * 1024 || fr < 48 * 1024) {
        ESP_LOGW(TAG, "Wersja: za malo pamieci na TLS (wolne %u B, blok %u B)", (unsigned)fr, (unsigned)big);
        return false;
    }
    esp_http_client_config_t cfg = {
        .url                         = UPD_URL,
        .timeout_ms                  = 15000,
        .crt_bundle_attach           = esp_crt_bundle_attach,
        .skip_cert_common_name_check = false,
        .buffer_size                 = 4096,
        .buffer_size_tx              = 1024,
        .user_agent                  = USER_AGENT,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0) {
        int st = esp_http_client_get_status_code(c);
        char loc[160];
        if ((st == 301 || st == 302 || st == 303 || st == 307 || st == 308) &&
            esp_http_client_set_redirection(c) == ESP_OK &&
            esp_http_client_get_url(c, loc, sizeof(loc)) == ESP_OK) {
            char tag[sizeof(s_upd_latest)];
            int r = upd_tag_from_url(loc, tag, sizeof(tag));
            if (r == 1) {
                strlcpy(s_upd_latest, tag, sizeof(s_upd_latest));
                s_upd_state = UPD_OK;
                ok = true;
            } else if (r == 0) {
                s_upd_latest[0] = 0;
                s_upd_state = UPD_NONE;
                ok = true;
            } else {
                ESP_LOGW(TAG, "Wersja: nieznany adres %s", loc);
            }
        } else {
            ESP_LOGW(TAG, "Wersja: GitHub odpowiedzial HTTP %d", st);
        }
    }
    esp_http_client_cleanup(c);
    return ok;
}

static void upd_task(void *arg) {
    (void)arg;
    bool ok = false;
    if (wifi_manager_get_state() == WIFI_STATE_CONNECTED && !ota_busy()) {
        esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
        ok = upd_fetch();
        if (ok && s_upd_state == UPD_OK)
            ESP_LOGI(TAG, "Wersja: najnowsza oficjalna %s, zainstalowana %s", s_upd_latest,
                     ota_get_running_version());
        else if (ok)
            ESP_LOGI(TAG, "Wersja: brak wydania oficjalnego na GitHub");
    }
    s_upd_err  = !ok;
    s_upd_busy = false;
    upd_schedule(ok ? UPD_PERIOD_S : UPD_RETRY_S);
    vTaskDelete(NULL);
}

static void upd_start_task(void) {
    if (s_upd_busy) return;
    s_upd_busy = true;
    // Zadanie tylko na czas sprawdzenia - stos na handshake TLS nie wisi caly czas
    if (xTaskCreate(upd_task, "upd_chk", 10240, NULL, 3, NULL) != pdPASS) {
        s_upd_busy = false;
        upd_schedule(UPD_RETRY_S);
    }
}

static void upd_timer_cb(void *arg) {
    (void)arg;
    if (!nvs_config_ptr()->upd_check) { upd_schedule(UPD_PERIOD_S); return; }
    upd_start_task();
}

void ota_update_check_now(void) { upd_start_task(); }

void ota_update_set_auto(bool on) {
    if (on) {
        upd_schedule(10);
    } else {
        // Wylaczone - plakietka znika; "Sprawdz teraz" nadal dziala
        s_upd_state = UPD_UNKNOWN;
        s_upd_err   = false;
        s_upd_latest[0] = 0;
    }
}

int ota_update_json(char *buf, size_t cap) {
    const char *st = s_upd_state == UPD_OK ? "ok" : (s_upd_state == UPD_NONE ? "none" : "unknown");
    char latest[sizeof(s_upd_latest)];
    strlcpy(latest, s_upd_latest, sizeof(latest));
    bool nw = s_upd_state == UPD_OK && ota_version_cmp(latest, ota_get_running_version()) > 0;
    return snprintf(buf, cap,
        "\"upd\":{\"auto\":%s,\"busy\":%s,\"state\":\"%s\",\"err\":%s,\"latest\":\"%s\",\"new\":%s}",
        nvs_config_ptr()->upd_check ? "true" : "false", s_upd_busy ? "true" : "false", st,
        s_upd_err ? "true" : "false", latest, nw ? "true" : "false");
}

void ota_manager_init(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY)
        ESP_LOGI(TAG, "Nowy firmware na probe (%s) - potwierdze po pelnym starcie", running->label);
    else
        ESP_LOGI(TAG, "Biezaca partycja: %s", running ? running->label : "?");
    const esp_timer_create_args_t ta = { .callback = upd_timer_cb, .name = "upd_chk" };
    if (esp_timer_create(&ta, &s_upd_timer) == ESP_OK) upd_schedule(UPD_FIRST_S);
}

void ota_manager_mark_valid(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return;
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    if (e == ESP_OK) ESP_LOGI(TAG, "Nowy firmware potwierdzony - powrot do poprzedniego wylaczony");
    else ESP_LOGW(TAG, "Potwierdzenie firmware nieudane: %s", esp_err_to_name(e));
}

ota_status_t ota_get_status(void) { return s_status; }

const char *ota_get_running_version(void) { return esp_app_get_description()->version; }

const char *ota_get_partition_label(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    return running ? running->label : "?";
}
