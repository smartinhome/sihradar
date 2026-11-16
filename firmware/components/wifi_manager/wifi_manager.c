#include "wifi_manager.h"
#include "captive_dns.h"
#include "nvs_config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Logika jak w SIHOS17: start w sieci domowej (STA), przy jej braku punkt
// dostepowy (AP) z portalem konfiguracyjnym. Nowosc: polaczenie z siecia
// wybrana w panelu jest sprawdzane na zywo, przy dzialajacym AP - panel
// pokazuje wynik (zle haslo / brak sieci / nowy adres modulu).

static const char *TAG = "WIFI";
static EventGroupHandle_t s_wifi_eg;
static wifi_state_t       s_state = WIFI_STATE_DISCONNECTED;
static int                s_rssi  = 0;
static char               s_ip[16] = "0.0.0.0";
static char               s_host[24];
static char               s_ap_ssid[24];
static esp_netif_t       *s_sta_netif = NULL;
static esp_netif_t       *s_ap_netif  = NULL;
// W trybie AP interfejs STA jest wlaczony (APSTA) tylko do skanowania i proby
// polaczenia z panelu - nie moze sam laczyc sie w kolko.
static bool               s_sta_autoconnect = false;
static volatile int       s_ap_clients = 0;
static int                s_retry = 0;
#define MAX_RETRY 5
// Utrata sieci w czasie pracy: proby z odstepami, po RUNTIME_MAX_RETRY restart
// (5 x 2 s + 15 x 10 s + 20 x 30 s = ok. 13 minut).
static bool               s_got_ip_once = false;
static int                s_runtime_retry = 0;
#define RUNTIME_MAX_RETRY 40
static esp_timer_handle_t s_reconn_timer = NULL;
// Tryb AP po nieudanym starcie nie moze byc stanem na zawsze: co minute
// sprawdzamy, czy zapisana siec domowa wrocila.
#define AP_RETRY_S 60
static esp_timer_handle_t s_ap_retry_timer = NULL;
static volatile bool      s_ap_try = false;
static int                s_ap_tick = 0;
static int                s_ap_try_ticks = 0;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// --- Proba polaczenia z panelu (tryb AP) ---
#define TEST_MAX_TRIES   3
#define TEST_TIMEOUT_S   40   // polaczono, ale brak adresu IP (DHCP) - poddajemy sie
#define TEST_RESTART_S   20   // czas dla panelu na pokazanie nowego adresu
static volatile wifi_test_t s_test = WIFI_TEST_IDLE;
static int                s_test_tries = 0;
static int                s_test_reason = 0;
static char               s_test_ssid[33];
static char               s_test_pass[65];
static char               s_test_ip[16];
static esp_timer_handle_t s_test_timer = NULL;

static void sta_idle(void) {
    wifi_config_t idle = {0};
    esp_wifi_set_config(WIFI_IF_STA, &idle);
}

static void timer_once(esp_timer_handle_t *t, esp_timer_cb_t cb, const char *name, uint64_t ms) {
    if (!*t) {
        const esp_timer_create_args_t a = { .callback = cb, .name = name };
        if (esp_timer_create(&a, t) != ESP_OK) return;
    }
    esp_timer_stop(*t);
    esp_timer_start_once(*t, ms * 1000ULL);
}

static void restart_fire(void *arg) {
    (void)arg;
    esp_restart();
}

static esp_timer_handle_t s_restart_timer = NULL;
static void restart_later(uint32_t ms) {
    timer_once(&s_restart_timer, restart_fire, "restart", ms);
}

static void test_timeout(void *arg) {
    (void)arg;
    if (s_test != WIFI_TEST_RUNNING) return;
    ESP_LOGW(TAG, "Proba: brak adresu IP w %d s - przerywam", TEST_TIMEOUT_S);
    s_test = WIFI_TEST_FAIL;
    s_test_reason = 0;
    esp_wifi_disconnect();
    sta_idle();
}

static uint32_t runtime_delay_ms(int n) {
    if (n <= 5)  return 2000;
    if (n <= 20) return 10000;
    return 30000;
}

static void reconn_fire(void *arg) {
    (void)arg;
    if (s_sta_autoconnect && s_state != WIFI_STATE_CONNECTED) esp_wifi_connect();
}

// Proba powrotu do sieci domowej z trybu AP. Przy podlaczonym kliencie AP
// (ktos konfiguruje modul) rzadziej - proba przestraja kanal i rozlacza go.
static void ap_retry_fire(void *arg) {
    (void)arg;
    if (s_state != WIFI_STATE_AP_MODE) return;
    // Panel prowadzi wlasna probe (albo juz sie udala i czeka na restart)
    if (s_test == WIFI_TEST_RUNNING || s_test == WIFI_TEST_OK) return;
    if (s_ap_try) {
        if (++s_ap_try_ticks >= 3) {
            s_ap_try = false;
            esp_wifi_disconnect();
            sta_idle();
        }
        return;
    }
    const sih_config_t *cfg = nvs_config_ptr();
    if (!cfg->wifi_ssid[0]) return;
    s_ap_tick++;
    if (s_ap_clients > 0 && (s_ap_tick % 5) != 0) return;
    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid,     cfg->wifi_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, cfg->wifi_pass, sizeof(sta_cfg.sta.password));
    if (esp_wifi_set_config(WIFI_IF_STA, &sta_cfg) != ESP_OK) return;
    s_ap_try = true;
    s_ap_try_ticks = 0;
    if (esp_wifi_connect() != ESP_OK) {
        s_ap_try = false;
        sta_idle();
        return;
    }
    ESP_LOGI(TAG, "Tryb AP: sprawdzam, czy siec \"%s\" wrocila", cfg->wifi_ssid);
}

static void sntp_start(void) {
    if (esp_sntp_enabled()) return;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_clients++;
        ESP_LOGI(TAG, "AP: klient podlaczony (razem %d)", s_ap_clients);
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) s_ap_clients--;
        ESP_LOGI(TAG, "AP: klient odlaczony (zostalo %d)", s_ap_clients);
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!s_sta_autoconnect) return;   // APSTA w trybie AP
        esp_wifi_connect();
        s_state = WIFI_STATE_CONNECTING;
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *dis = (wifi_event_sta_disconnected_t *)data;
        int reason = dis ? dis->reason : 0;
        ESP_LOGW(TAG, "Rozlaczono z Wi-Fi, powod = %d", reason);
        if (s_test == WIFI_TEST_RUNNING) {
            if (++s_test_tries < TEST_MAX_TRIES) {
                ESP_LOGI(TAG, "Proba %d/%d polaczenia z \"%s\"", s_test_tries + 1, TEST_MAX_TRIES, s_test_ssid);
                esp_wifi_connect();
            } else {
                s_test = WIFI_TEST_FAIL;
                s_test_reason = reason;
                if (s_test_timer) esp_timer_stop(s_test_timer);
                sta_idle();
                ESP_LOGW(TAG, "Nie udalo sie polaczyc z \"%s\" (powod %d)", s_test_ssid, reason);
            }
            return;
        }
        if (s_ap_try) {
            s_ap_try = false;
            sta_idle();
            ESP_LOGI(TAG, "Tryb AP: sieci domowej nadal brak - kolejna proba za %d s", AP_RETRY_S);
            return;
        }
        if (!s_sta_autoconnect) return;
        if (s_got_ip_once) {
            s_runtime_retry++;
            s_state = WIFI_STATE_CONNECTING;
            if (s_runtime_retry <= RUNTIME_MAX_RETRY) {
                uint32_t ms = runtime_delay_ms(s_runtime_retry);
                ESP_LOGW(TAG, "Ponowna proba %d/%d za %u s", s_runtime_retry, RUNTIME_MAX_RETRY,
                         (unsigned)(ms / 1000));
                timer_once(&s_reconn_timer, reconn_fire, "sta_retry", ms);
            } else {
                ESP_LOGW(TAG, "Brak sieci od ok. 13 minut - restart");
                esp_restart();
            }
        } else if (s_retry < MAX_RETRY) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGW(TAG, "Ponowne laczenie... (%d/%d)", s_retry, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
            s_state = WIFI_STATE_DISCONNECTED;
            ESP_LOGE(TAG, "Nie mozna polaczyc - przelaczam na AP");
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        if (s_test == WIFI_TEST_RUNNING) {
            // Sukces proby z panelu: zapis i restart do pracy w sieci domowej.
            // AP dziala jeszcze TEST_RESTART_S sekund - panel pokaze nowy adres.
            if (s_test_timer) esp_timer_stop(s_test_timer);
            snprintf(s_test_ip, sizeof(s_test_ip), IPSTR, IP2STR(&ev->ip_info.ip));
            sih_config_t cfg = nvs_config_get();
            strlcpy(cfg.wifi_ssid, s_test_ssid, sizeof(cfg.wifi_ssid));
            strlcpy(cfg.wifi_pass, s_test_pass, sizeof(cfg.wifi_pass));
            nvs_config_save(&cfg);
            s_test = WIFI_TEST_OK;
            ESP_LOGI(TAG, "Polaczono z \"%s\", adres %s - restart za %d s",
                     s_test_ssid, s_test_ip, TEST_RESTART_S);
            restart_later(TEST_RESTART_S * 1000);
            return;
        }
        if (s_state == WIFI_STATE_AP_MODE) {
            // Siec domowa wrocila w czasie pracy w AP - czysty start w STA
            s_ap_try = false;
            ESP_LOGI(TAG, "Siec domowa wrocila - restart do pracy w sieci");
            esp_restart();
            return;
        }
        if (s_reconn_timer) esp_timer_stop(s_reconn_timer);
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
        s_runtime_retry = 0;
        s_got_ip_once = true;
        s_state = WIFI_STATE_CONNECTED;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Polaczono: %s (http://%s.local)", s_ip, s_host);
        sntp_start();
    }
}

static void mdns_start(void) {
    static bool started = false;
    if (started) return;
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS nie wystartowal");
        return;
    }
    started = true;
    mdns_hostname_set(s_host);
    mdns_instance_name_set("SIH Radar");
    mdns_service_add("SIH Radar", "_http", "_tcp", 80, NULL, 0);
}

static void start_ap(const sih_config_t *cfg) {
    s_sta_autoconnect = false;
    s_ap_clients = 0;
    if (!s_ap_netif)  s_ap_netif  = esp_netif_create_default_wifi_ap();
    if (!s_sta_netif) s_sta_netif = esp_netif_create_default_wifi_sta();
    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid,     s_ap_ssid,    sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, cfg->ap_pass, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = strlen(s_ap_ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    // APSTA: skanowanie i proba polaczenia wymagaja interfejsu STA
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    // Jawnie pusta konfiguracja STA - inaczej sterownik co chwile szuka sieci
    // i zabiera czas antenie wspoldzielonej z AP.
    sta_idle();
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    s_state = WIFI_STATE_AP_MODE;
    esp_wifi_disconnect();
    // Sterownik mimo pustej konfiguracji STA zasypuje log "Haven't to connect..."
    esp_log_level_set("wifi", ESP_LOG_ERROR);

    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
        captive_dns_start(ip.ip.addr);
        ESP_LOGI(TAG, "Tryb AP: %s (haslo %s), panel: http://" IPSTR "/",
                 s_ap_ssid, cfg->ap_pass, IP2STR(&ip.ip));
    }
    mdns_start();

    if (cfg->wifi_ssid[0]) {
        if (!s_ap_retry_timer) {
            const esp_timer_create_args_t a = { .callback = ap_retry_fire, .name = "ap_retry" };
            esp_timer_create(&a, &s_ap_retry_timer);
        }
        if (s_ap_retry_timer) {
            esp_timer_stop(s_ap_retry_timer);
            esp_timer_start_periodic(s_ap_retry_timer, (uint64_t)AP_RETRY_S * 1000000ULL);
            ESP_LOGI(TAG, "Tryb AP: co %d s sprawdzam siec \"%s\"", AP_RETRY_S, cfg->wifi_ssid);
        }
    }
}

void wifi_manager_init(void) {
    s_wifi_eg = xEventGroupCreate();
    const sih_config_t *cfg = nvs_config_ptr();

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_host, sizeof(s_host), "sih-radar-%02x%02x", mac[4], mac[5]);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "SIH-Radar-%02X%02X", mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    if (strlen(cfg->wifi_ssid) == 0) {
        ESP_LOGW(TAG, "Brak zapisanej sieci - start w trybie AP");
        start_ap(cfg);
        return;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta_netif, s_host);
    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid,     cfg->wifi_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, cfg->wifi_pass, sizeof(sta_cfg.sta.password));

    s_sta_autoconnect = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Bez oszczedzania energii - modul zasilany z sieci, a power save na C6
    // bywa przyczyna losowych rozlaczen z niektorymi routerami.
    esp_wifi_set_ps(WIFI_PS_NONE);
    mdns_start();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "Brak sieci domowej - przechodze w tryb AP");
        esp_wifi_stop();
        start_ap(cfg);
    }
}

wifi_state_t wifi_manager_get_state(void) { return s_state; }
wifi_test_t  wifi_manager_test_state(void) { return s_test; }
int          wifi_manager_ap_clients(void) { return s_ap_clients; }
const char  *wifi_manager_hostname(void) { return s_host; }
const char  *wifi_manager_ap_ssid(void) { return s_ap_ssid; }

int wifi_manager_get_rssi(void) {
    wifi_ap_record_t ap;
    if (s_state == WIFI_STATE_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s_rssi = ap.rssi;
    return s_rssi;
}

int wifi_manager_get_channel(void) {
    wifi_ap_record_t ap;
    if (s_state != WIFI_STATE_CONNECTED || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.primary;
}

void wifi_manager_get_ip(char *buf, size_t len) {
    if (s_state == WIFI_STATE_AP_MODE && s_ap_netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK && ip.ip.addr) {
            snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
            return;
        }
    }
    strlcpy(buf, s_ip, len);
}

static int json_str(char *out, int cap, const char *s) {
    int e = 0;
    for (int k = 0; s[k] && e < cap - 2; k++) {
        unsigned char c = (unsigned char)s[k];
        if (c < 0x20) continue;
        if (c == '"' || c == '\\') out[e++] = '\\';
        out[e++] = (char)c;
    }
    out[e] = 0;
    return e;
}

// Skan nie przelacza trybu: w AP modul pracuje w APSTA od startu, a
// przelaczanie rozlaczaloby telefon, ktory wlasnie pyta o liste sieci.
int wifi_manager_scan(char *json_buf, int cap) {
    if (s_test == WIFI_TEST_RUNNING) {
        snprintf(json_buf, cap, "[]");
        return -1;
    }
    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 40, .max = 90 } },
    };
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        ESP_LOGW(TAG, "Skanowanie nieudane");
        snprintf(json_buf, cap, "[]");
        return -1;
    }
    uint16_t cnt = 0;
    esp_wifi_scan_get_ap_num(&cnt);
    if (cnt > 24) cnt = 24;
    if (cnt == 0) {
        esp_wifi_clear_ap_list();
        snprintf(json_buf, cap, "[]");
        return 0;
    }
    wifi_ap_record_t *recs = calloc(cnt, sizeof(wifi_ap_record_t));
    if (!recs) {
        esp_wifi_clear_ap_list();
        snprintf(json_buf, cap, "[]");
        return -1;
    }
    uint16_t got = cnt;
    esp_wifi_scan_get_ap_records(&got, recs);

    // Wyniki sa posortowane po sile sygnalu - przy powtorzonym SSID
    // (kilka nadajnikow jednej sieci) zostaje najsilniejszy.
    int n = snprintf(json_buf, cap, "[");
    int written = 0;
    for (int i = 0; i < got; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp((const char *)recs[j].ssid, ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;
        char esc[72];
        json_str(esc, sizeof(esc), ssid);
        n += snprintf(json_buf + n, cap - n, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d,\"ch\":%d}",
                      written ? "," : "", esc, recs[i].rssi, recs[i].authmode, recs[i].primary);
        written++;
        if (n > cap - 100) break;
    }
    snprintf(json_buf + n, cap - n, "]");
    free(recs);
    return written;
}

const char *wifi_manager_connect(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0]) return "Podaj nazwę sieci.";
    if (strlen(ssid) > 32) return "Nazwa sieci jest za długa (maks. 32 znaki).";
    if (!pass) pass = "";
    size_t pl = strlen(pass);
    if (pl > 64) return "Hasło jest za długie (maks. 64 znaki).";
    if (pl > 0 && pl < 8) return "Hasło sieci Wi-Fi ma co najmniej 8 znaków.";

    if (s_state != WIFI_STATE_AP_MODE) {
        // Zmiana sieci domowej: zapis i restart (nie da sie sprawdzic nowej
        // sieci, nie zrywajac polaczenia, przez ktore pyta przegladarka).
        sih_config_t cfg = nvs_config_get();
        strlcpy(cfg.wifi_ssid, ssid, sizeof(cfg.wifi_ssid));
        strlcpy(cfg.wifi_pass, pass, sizeof(cfg.wifi_pass));
        if (!nvs_config_save(&cfg)) return "Nie udało się zapisać ustawień.";
        ESP_LOGI(TAG, "Nowa siec \"%s\" zapisana - restart", ssid);
        restart_later(1500);
        return NULL;
    }

    if (s_test == WIFI_TEST_RUNNING) return "Trwa już próba połączenia.";
    if (s_test == WIFI_TEST_OK) return "Moduł jest już połączony i zaraz się zrestartuje.";
    strlcpy(s_test_ssid, ssid, sizeof(s_test_ssid));
    strlcpy(s_test_pass, pass, sizeof(s_test_pass));
    s_test_ip[0] = 0;
    s_test_tries = 0;
    s_test_reason = 0;

    if (s_ap_try) {          // przerwij cicha probe powrotu do starej sieci
        s_ap_try = false;
        esp_wifi_disconnect();
    }
    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid,     s_test_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, s_test_pass, sizeof(sta_cfg.sta.password));
    if (esp_wifi_set_config(WIFI_IF_STA, &sta_cfg) != ESP_OK) return "Błąd konfiguracji Wi-Fi.";
    esp_netif_set_hostname(s_sta_netif, s_host);
    s_test = WIFI_TEST_RUNNING;
    if (esp_wifi_connect() != ESP_OK) {
        s_test = WIFI_TEST_IDLE;
        sta_idle();
        return "Nie udało się rozpocząć łączenia.";
    }
    timer_once(&s_test_timer, test_timeout, "wifi_test", TEST_TIMEOUT_S * 1000ULL);
    ESP_LOGI(TAG, "Proba polaczenia z \"%s\" z panelu", ssid);
    return NULL;
}

int wifi_manager_test_json(char *buf, int cap) {
    static const char *names[] = { "idle", "running", "ok", "fail" };
    char esc[72];
    json_str(esc, sizeof(esc), s_test_ssid);
    return snprintf(buf, cap,
        "{\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"reason\":%d,\"try\":%d,"
        "\"host\":\"%s\",\"restart_s\":%d}",
        names[s_test], esc, s_test_ip, s_test_reason, s_test_tries + 1,
        s_host, TEST_RESTART_S);
}
