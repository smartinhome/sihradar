#include "api_handlers.h"
#include "wifi_manager.h"
#include "nvs_config.h"
#include "ld2450.h"
#include "bme688.h"
#include "ota_manager.h"
#include "room.h"
#include "presence.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "driver/temperature_sensor.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "API";
static httpd_handle_t s_srv = NULL;
static temperature_sensor_handle_t s_tsens = NULL;

#define WS_PERIOD_MS 100      // radar daje ok. 10 ramek na sekunde
#define JSON_CAP     1536

// --------------------------------------------------------------------------
// Pomocnicze
// --------------------------------------------------------------------------

static void json_esc(char *out, size_t cap, const char *s) {
    size_t e = 0;
    for (size_t k = 0; s[k] && e + 2 < cap; k++) {
        unsigned char c = (unsigned char)s[k];
        if (c < 0x20) continue;
        if (c == '"' || c == '\\') out[e++] = '\\';
        out[e++] = (char)c;
    }
    out[e] = 0;
}

static esp_err_t send_json(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_err(httpd_req_t *req, const char *status, const char *msg) {
    char esc[160], buf[200];
    json_esc(esc, sizeof(esc), msg);
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", esc);
    httpd_resp_set_status(req, status);
    return send_json(req, buf);
}

// Cialo zapytania POST jako JSON (maly limit - to tylko formularze panelu;
// najwiekszy jest zapis stref)
#define BODY_MAX 2048
static cJSON *read_json(httpd_req_t *req) {
    if (req->content_len <= 0 || req->content_len > BODY_MAX) return NULL;
    char *buf = malloc(req->content_len + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[got] = 0;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

static float chip_temp(void) {
    float t = 0;
    if (s_tsens && temperature_sensor_get_celsius(s_tsens, &t) == ESP_OK) return t;
    return -127;
}

// Dane czujnikow - ten sam JSON dla /api/sensors i WebSocket /ws
static int sensors_json(char *buf, int cap) {
    ld2450_data_t r;
    bme688_data_t b;
    ld2450_get(&r);
    bme688_get(&b);
    char fw[48];
    json_esc(fw, sizeof(fw), r.fw);
    char hex[sizeof(r.raw) * 3 + 1] = "";
    for (int i = 0; i < r.raw_len; i++)
        snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02X ", r.raw[i]);
    if (r.raw_len) hex[r.raw_len * 3 - 1] = 0;
    // Cele po filtrach modulu: wygladzone, chwilowo zgubione przytrzymane
    // ("h"), nowe czekaja na potwierdzenie (stan w "pres":{"ts":[..]})
    pz_target_t pt[PZ_TARGETS];
    presence_targets(pt);
    unsigned cnt = 0;
    for (int i = 0; i < PZ_TARGETS; i++) {
        if (!r.online) pt[i].valid = false;   // radar milczy - nie pokazujemy starych pozycji
        if (pt[i].valid) cnt++;
    }
    int n = snprintf(buf, cap,
        "{\"up\":%lld,\"radar\":{\"online\":%s,\"fw\":\"%s\",\"fps\":%u,\"count\":%u,"
        "\"frames\":%lu,\"errors\":%lu,\"baud\":%lu,\"scan\":%s,\"rx\":%lu,\"raw\":\"%s\",\"targets\":[",
        (long long)(esp_timer_get_time() / 1000), r.online ? "true" : "false", fw,
        r.fps, cnt, (unsigned long)r.frames, (unsigned long)r.errors,
        (unsigned long)r.baud, r.scanning ? "true" : "false", (unsigned long)r.rx_bytes, hex);
    bool first = true;
    for (int i = 0; i < PZ_TARGETS && n < cap; i++) {
        if (!pt[i].valid) continue;
        n += snprintf(buf + n, cap - n, "%s{\"id\":%d,\"x\":%d,\"y\":%d,\"v\":%d,\"res\":%u,\"h\":%d}",
                      first ? "" : ",", i + 1, pt[i].rx, pt[i].ry, pt[i].speed,
                      r.t[i].valid ? r.t[i].res_mm : 0, pt[i].held);
        first = false;
    }
    if (n < cap) {
        n += snprintf(buf + n, cap - n,
            "]},\"bme\":{\"online\":%s,\"chip\":\"%s\",\"addr\":%u,\"t\":%.2f,\"h\":%.1f,"
            "\"p\":%.1f,\"gas\":%.1f,\"gas_ok\":%s,\"n\":%lu},\"pres\":",
            b.online ? "true" : "false", b.chip, b.addr, b.temp_c, b.hum_pct, b.press_hpa,
            b.gas_kohm, b.gas_valid ? "true" : "false", (unsigned long)b.samples);
    }
    if (n < cap) n += presence_json(buf + n, cap - n);
    if (n < cap) n += snprintf(buf + n, cap - n, "}");
    return n < cap ? n : cap - 1;
}

// --------------------------------------------------------------------------
// REST
// --------------------------------------------------------------------------

static esp_err_t h_status(httpd_req_t *req) {
    const esp_app_desc_t *app = esp_app_get_description();
    const sih_config_t *cfg = nvs_config_ptr();
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char ip[16];
    wifi_manager_get_ip(ip, sizeof(ip));
    wifi_state_t st = wifi_manager_get_state();
    const char *mode = st == WIFI_STATE_CONNECTED ? "sta" :
                       st == WIFI_STATE_AP_MODE   ? "ap"  :
                       st == WIFI_STATE_CONNECTING ? "connecting" : "off";
    char ssid[72];
    json_esc(ssid, sizeof(ssid), cfg->wifi_ssid);
    char ver[72];
    json_esc(ver, sizeof(ver), app->version);
    char buf[900];
    int n = snprintf(buf, sizeof(buf),
        "{\"fw\":\"%s\",\"build\":\"%s %s\",\"idf\":\"%s\",\"uptime\":%lld,"
        "\"heap\":%lu,\"heap_min\":%lu,\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"chip_temp\":%.1f,"
        "\"part\":\"%s\","
        "\"wifi\":{\"mode\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"ch\":%d,"
        "\"host\":\"%s\",\"ap_ssid\":\"%s\",\"ap_clients\":%d},",
        ver, app->date, app->time, app->idf_ver,
        (long long)(esp_timer_get_time() / 1000000),
        (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size(),
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], chip_temp(),
        ota_get_partition_label(),
        mode, ssid, ip, wifi_manager_get_rssi(), wifi_manager_get_channel(),
        wifi_manager_hostname(), wifi_manager_ap_ssid(), wifi_manager_ap_clients());
    n += ota_update_json(buf + n, sizeof(buf) - n);
    snprintf(buf + n, sizeof(buf) - n, "}");
    return send_json(req, buf);
}

static esp_err_t h_sensors(httpd_req_t *req) {
    char *buf = malloc(JSON_CAP);
    if (!buf) return send_err(req, "500 Internal Server Error", "Brak pamięci");
    sensors_json(buf, JSON_CAP);
    esp_err_t r = send_json(req, buf);
    free(buf);
    return r;
}

static esp_err_t h_wifi_scan(httpd_req_t *req) {
    char *buf = malloc(3072);
    if (!buf) return send_err(req, "500 Internal Server Error", "Brak pamięci");
    wifi_manager_scan(buf, 3072);
    esp_err_t r = send_json(req, buf);
    free(buf);
    return r;
}

static esp_err_t h_wifi_connect(httpd_req_t *req) {
    cJSON *j = read_json(req);
    if (!j) return send_err(req, "400 Bad Request", "Nieprawidłowe dane");
    const cJSON *s = cJSON_GetObjectItem(j, "ssid");
    const cJSON *p = cJSON_GetObjectItem(j, "password");
    const char *ssid = cJSON_IsString(s) ? s->valuestring : "";
    const char *pass = cJSON_IsString(p) ? p->valuestring : "";
    bool ap = wifi_manager_get_state() == WIFI_STATE_AP_MODE;
    const char *err = wifi_manager_connect(ssid, pass);
    cJSON_Delete(j);
    if (err) return send_err(req, "400 Bad Request", err);
    // ap=true: panel sledzi probe przez /api/wifi/test; false: modul restartuje sie
    return send_json(req, ap ? "{\"ok\":true,\"test\":true}" : "{\"ok\":true,\"test\":false}");
}

static esp_err_t h_wifi_test(httpd_req_t *req) {
    char buf[256];
    wifi_manager_test_json(buf, sizeof(buf));
    return send_json(req, buf);
}

// --------------------------------------------------------------------------
// Plan pomieszczenia
// --------------------------------------------------------------------------

static esp_err_t h_room_get(httpd_req_t *req) {
    char buf[200];
    room_json(buf, sizeof(buf));
    return send_json(req, buf);
}

// {"w":4200,"l":3600,"x":2100,"y":0,"rot":0,"flip":false} - wszystko w mm i stopniach
static esp_err_t h_room_set(httpd_req_t *req) {
    cJSON *j = read_json(req);
    if (!j) return send_err(req, "400 Bad Request", "Nieprawidłowe dane");
    static const char *keys[] = { "w", "l", "x", "y", "rot" };
    double v[5];
    for (int i = 0; i < 5; i++) {
        const cJSON *it = cJSON_GetObjectItem(j, keys[i]);
        if (!cJSON_IsNumber(it)) {
            cJSON_Delete(j);
            return send_err(req, "400 Bad Request", "Brak wymiarów lub położenia radaru");
        }
        v[i] = it->valuedouble;
    }
    room_t r = {
        .w_mm = (int32_t)v[0], .l_mm = (int32_t)v[1],
        .x_mm = (int32_t)v[2], .y_mm = (int32_t)v[3],
        .rot_deg = (int16_t)v[4],
        .flip = cJSON_IsTrue(cJSON_GetObjectItem(j, "flip")),
    };
    cJSON_Delete(j);
    const char *err = room_save(&r);
    if (err) return send_err(req, "400 Bad Request", err);
    return h_room_get(req);
}

// --------------------------------------------------------------------------
// Strefy i ustawienia wykrywania obecnosci
// --------------------------------------------------------------------------

#define ZONES_CAP 1400

static esp_err_t h_zones_get(httpd_req_t *req) {
    char *buf = malloc(ZONES_CAP);
    if (!buf) return send_err(req, "500 Internal Server Error", "Brak pamięci");
    presence_zones_json(buf, ZONES_CAP);
    esp_err_t r = send_json(req, buf);
    free(buf);
    return r;
}

static bool num(const cJSON *o, const char *k, double lo, double hi, double *out) {
    const cJSON *it = cJSON_GetObjectItem(o, k);
    if (!cJSON_IsNumber(it) || it->valuedouble < lo || it->valuedouble > hi) return false;
    *out = it->valuedouble;
    return true;
}

// Kopiuje nazwe strefy, ucinajac na granicy znaku UTF-8
static void copy_name(char *dst, const char *src) {
    strlcpy(dst, src, PZ_NAME_LEN);
    size_t L = strlen(dst);
    size_t k = L;
    while (k && ((unsigned char)dst[k - 1] & 0xC0) == 0x80) k--;   // bajty kontynuacji
    if (k && ((unsigned char)dst[k - 1] & 0x80)) {
        unsigned char lead = (unsigned char)dst[k - 1];
        size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
        if (L - (k - 1) < need) dst[k - 1] = 0;   // znak uciety w polowie
    }
}

// {"cfg":{"dist":6000,"hold":30,"zoff":5,"latch":30,"exit":400,"mem":true,"room":true},
//  "zones":[{"name":"Kanapa","t":1,"x1":0,"y1":0,"x2":1000,"y2":800}]}  (t: 1 obecnosc, 2 pomijaj)
static esp_err_t h_zones_set(httpd_req_t *req) {
    cJSON *j = read_json(req);
    if (!j) return send_err(req, "400 Bad Request", "Nieprawidłowe dane");
    const char *err = NULL;
    pz_cfg_t c = presence_get_cfg();
    const cJSON *jc = cJSON_GetObjectItem(j, "cfg");
    if (cJSON_IsObject(jc)) {
        double v;
        if (num(jc, "dist", 0, 65535, &v))  c.max_dist_mm = (uint16_t)v;
        if (num(jc, "hold", 0, 65535, &v))  c.hold_s = (uint16_t)v;
        if (num(jc, "zoff", 0, 65535, &v))  c.zone_off_s = (uint16_t)v;
        if (num(jc, "latch", 0, 65535, &v)) c.latch_min = (uint16_t)v;
        if (num(jc, "exit", 0, 65535, &v))  c.exit_mm = (uint16_t)v;
        const cJSON *b = cJSON_GetObjectItem(jc, "mem");
        if (cJSON_IsBool(b)) c.memory = cJSON_IsTrue(b);
        b = cJSON_GetObjectItem(jc, "room");
        if (cJSON_IsBool(b)) c.room_only = cJSON_IsTrue(b);
    }
    static pz_zone_t zs[PZ_MAX_ZONES];     // serwer HTTP obsluguje jedno zapytanie naraz
    memset(zs, 0, sizeof(zs));
    int n = 0;
    const cJSON *ja = cJSON_GetObjectItem(j, "zones");
    if (cJSON_IsArray(ja)) {
        const cJSON *z;
        cJSON_ArrayForEach(z, ja) {
            if (n >= PZ_MAX_ZONES) { err = "Najwyżej 8 stref."; break; }
            double x1, y1, x2, y2, t;
            if (!num(z, "x1", -32768, 32767, &x1) || !num(z, "y1", -32768, 32767, &y1) ||
                !num(z, "x2", -32768, 32767, &x2) || !num(z, "y2", -32768, 32767, &y2) ||
                !num(z, "t", 0, 255, &t)) { err = "Niepełne dane strefy."; break; }
            zs[n] = (pz_zone_t){ .x1 = (int16_t)x1, .y1 = (int16_t)y1, .x2 = (int16_t)x2,
                                 .y2 = (int16_t)y2, .type = (uint8_t)t };
            const cJSON *nm = cJSON_GetObjectItem(z, "name");
            if (cJSON_IsString(nm)) copy_name(zs[n].name, nm->valuestring);
            n++;
        }
    }
    cJSON_Delete(j);
    if (!err) err = presence_save(&c, zs, n);
    if (err) return send_err(req, "400 Bad Request", err);
    return h_zones_get(req);
}

// {"action":"restart"|"factory"} - serwis radaru
static esp_err_t h_radar(httpd_req_t *req) {
    cJSON *j = read_json(req);
    if (!j) return send_err(req, "400 Bad Request", "Nieprawidłowe dane");
    const cJSON *a = cJSON_GetObjectItem(j, "action");
    int rq = 0;
    if (cJSON_IsString(a) && strcmp(a->valuestring, "restart") == 0) rq = LD2450_REQ_RESTART;
    if (cJSON_IsString(a) && strcmp(a->valuestring, "factory") == 0) rq = LD2450_REQ_FACTORY;
    cJSON_Delete(j);
    if (!rq) return send_err(req, "400 Bad Request", "Nieznane polecenie");
    ld2450_request((ld2450_req_t)rq);
    return send_json(req, "{\"ok\":true}");
}

// --------------------------------------------------------------------------
// Aktualizacje (jak w SIHOS17)
// --------------------------------------------------------------------------

// ?channel=beta&tag=v0.1.0-beta3 albo bez parametrow = kanal oficjalny
static esp_err_t h_ota_github(httpd_req_t *req) {
    if (wifi_manager_get_state() != WIFI_STATE_CONNECTED)
        return send_err(req, "409 Conflict", "Moduł nie ma dostępu do internetu - wgraj plik .bin");
    if (ota_busy()) return send_err(req, "409 Conflict", "Aktualizacja już trwa");
    bool beta = false;
    char query[128], tag[48] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(query, "channel", val, sizeof(val)) == ESP_OK)
            beta = (strcmp(val, "beta") == 0);
        if (httpd_query_key_value(query, "tag", tag, sizeof(tag)) != ESP_OK) tag[0] = 0;
    }
    ota_start_from_github(beta, tag);
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t h_ota_url(httpd_req_t *req) {
    cJSON *j = read_json(req);
    if (!j) return send_err(req, "400 Bad Request", "Nieprawidłowe dane");
    const cJSON *u = cJSON_GetObjectItem(j, "url");
    char url[480] = {0};
    if (cJSON_IsString(u)) strlcpy(url, u->valuestring, sizeof(url));
    cJSON_Delete(j);
    if (!ota_url_allowed(url))
        return send_err(req, "400 Bad Request",
                        "Dozwolone tylko adresy https:// z github.com lub www.smartinhome.pl");
    if (wifi_manager_get_state() != WIFI_STATE_CONNECTED)
        return send_err(req, "409 Conflict", "Moduł nie ma dostępu do internetu - wgraj plik .bin");
    if (ota_busy()) return send_err(req, "409 Conflict", "Aktualizacja już trwa");
    ota_start_from_url(url);
    return send_json(req, "{\"ok\":true}");
}

static void restart_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS((uint32_t)(uintptr_t)arg));
    esp_restart();
}

// Plik .bin z przegladarki zapisywany STRUMIENIOWO wprost do partycji OTA.
// Jedyna droga aktualizacji w trybie AP, gdzie modul nie ma internetu.
static esp_err_t h_ota_upload(httpd_req_t *req) {
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    size_t len = req->content_len;
    if (!part) return send_err(req, "500 Internal Server Error", "Brak partycji OTA");
    if (len < ota_image_header_len() || len > part->size)
        return send_err(req, "400 Bad Request", "Nieprawidłowy rozmiar pliku");
    if (ota_busy()) return send_err(req, "409 Conflict", "Aktualizacja już trwa");

    const int CH = 4096;
    char *buf = malloc(CH);
    if (!buf) return send_err(req, "500 Internal Server Error", "Brak pamięci");
    ota_set_upload_busy(true);

    esp_ota_handle_t h = 0;
    bool open = false;
    const char *err = NULL;
    size_t got = 0;
    int fill = 0;
    int hdr = (int)ota_image_header_len();
    while (got < len) {
        int want = CH - fill;
        if ((size_t)want > len - got) want = (int)(len - got);
        int r = httpd_req_recv(req, buf + fill, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { err = "Przerwano przesyłanie pliku"; break; }
        got += r;
        fill += r;
        if (!open) {
            if (fill < hdr && got < len) continue;   // czekamy na caly naglowek
            err = ota_check_image_header((const uint8_t *)buf, fill);
            if (err) break;
            if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
                err = "Nie udało się rozpocząć zapisu";
                break;
            }
            open = true;
        }
        if (esp_ota_write(h, buf, fill) != ESP_OK) { err = "Błąd zapisu do pamięci flash"; break; }
        fill = 0;
    }
    free(buf);

    if (!err && open && esp_ota_end(h) != ESP_OK) { err = "Plik uszkodzony (zła suma kontrolna)"; open = false; }
    else if (err && open) esp_ota_abort(h);
    if (!err && esp_ota_set_boot_partition(part) != ESP_OK) err = "Nie można ustawić nowej partycji";
    ota_set_upload_busy(false);
    if (err) {
        ESP_LOGW(TAG, "Wgrywanie pliku: %s", err);
        return send_err(req, "400 Bad Request", err);
    }
    ESP_LOGI(TAG, "Wgrano plik %u B do %s - restart", (unsigned)got, part->label);
    send_json(req, "{\"ok\":true}");
    xTaskCreate(restart_task, "restart", 2048, (void *)(uintptr_t)1000, 5, NULL);
    return ESP_OK;
}

static esp_err_t h_ota_status(httpd_req_t *req) {
    static const char *states[] = { "idle", "downloading", "writing", "success", "failed", "uptodate" };
    ota_status_t st = ota_get_status();
    char esc[96], buf[200];
    json_esc(esc, sizeof(esc), st.error);
    snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"progress\":%d,\"error\":\"%s\"}",
             states[st.state], st.progress_pct, esc);
    return send_json(req, buf);
}

// {"auto":true|false} - przelacznik sprawdzania, {"action":"check"} - sprawdz teraz
static esp_err_t h_update(httpd_req_t *req) {
    cJSON *j = read_json(req);
    if (!j) return send_err(req, "400 Bad Request", "Nieprawidłowe dane");
    const cJSON *a = cJSON_GetObjectItem(j, "action");
    const cJSON *au = cJSON_GetObjectItem(j, "auto");
    esp_err_t r;
    if (cJSON_IsString(a) && strcmp(a->valuestring, "check") == 0) {
        ota_update_check_now();
        r = send_json(req, "{\"ok\":true}");
    } else if (cJSON_IsBool(au)) {
        sih_config_t cfg = nvs_config_get();
        cfg.upd_check = cJSON_IsTrue(au);
        nvs_config_save(&cfg);
        ota_update_set_auto(cfg.upd_check);
        r = send_json(req, "{\"ok\":true}");
    } else {
        r = send_err(req, "400 Bad Request", "Nieznane polecenie");
    }
    cJSON_Delete(j);
    return r;
}

static void reboot_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t h_reboot(httpd_req_t *req) {
    send_json(req, "{\"ok\":true}");
    ESP_LOGI(TAG, "Restart z panelu");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t h_favicon(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

// Wszystko inne. W trybie AP przekierowanie na panel: telefon sprawdza
// lacznosc pod wlasnymi adresami (/generate_204, /hotspot-detect.html...),
// a 302 zamiast oczekiwanej odpowiedzi otwiera okno "Zaloguj sie do sieci".
static esp_err_t h_any(httpd_req_t *req) {
    if (wifi_manager_get_state() == WIFI_STATE_AP_MODE && strncmp(req->uri, "/api/", 5) != 0) {
        char ip[16], loc[40];
        wifi_manager_get_ip(ip, sizeof(ip));
        snprintf(loc, sizeof(loc), "http://%s/", ip);
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", loc);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, NULL, 0);
    }
    return send_err(req, "404 Not Found", "Nie ma takiej strony");
}

// --------------------------------------------------------------------------
// WebSocket /ws - dane czujnikow na zywo (panel nie musi odpytywac co 100 ms)
// --------------------------------------------------------------------------

typedef struct {
    int    fd;
    size_t len;
    char   data[];
} ws_job_t;

// Zadania czekajace w kolejce serwera. Gdy serwer jest zajety (np. przyjmuje
// plik aktualizacji), nie dokladamy kolejnych - kazde to kopia JSON-a w RAM.
static volatile int s_ws_pending = 0;
#define WS_MAX_PENDING 6

static void ws_send_work(void *arg) {
    ws_job_t *j = (ws_job_t *)arg;
    s_ws_pending--;
    httpd_ws_frame_t f = {
        .final = true,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)j->data,
        .len = j->len,
    };
    if (httpd_ws_send_frame_async(s_srv, j->fd, &f) != ESP_OK) {
        httpd_sess_trigger_close(s_srv, j->fd);   // klient zniknal (np. telefon uspiony)
    }
    free(j);
}

static void ws_task(void *arg) {
    (void)arg;
    char *buf = malloc(JSON_CAP);
    if (!buf) { vTaskDelete(NULL); return; }
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(WS_PERIOD_MS));
        if (ota_busy()) continue;   // pamiec i siec dla aktualizacji
        int fds[CONFIG_LWIP_MAX_SOCKETS];
        size_t n = CONFIG_LWIP_MAX_SOCKETS;
        if (httpd_get_client_list(s_srv, &n, fds) != ESP_OK) continue;
        int len = -1;
        for (size_t i = 0; i < n; i++) {
            if (httpd_ws_get_fd_info(s_srv, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
            if (s_ws_pending >= WS_MAX_PENDING) break;
            if (len < 0) len = sensors_json(buf, JSON_CAP);
            ws_job_t *j = malloc(sizeof(ws_job_t) + len + 1);
            if (!j) break;
            j->fd = fds[i];
            j->len = len;
            memcpy(j->data, buf, len + 1);
            s_ws_pending++;
            if (httpd_queue_work(s_srv, ws_send_work, j) != ESP_OK) { s_ws_pending--; free(j); }
        }
    }
}

static esp_err_t h_ws(httpd_req_t *req) {
    if (req->method == HTTP_GET) return ESP_OK;   // handshake
    // Panel nic nie wysyla - odbieramy tylko, zeby oproznic gniazdo
    httpd_ws_frame_t f = {0};
    esp_err_t r = httpd_ws_recv_frame(req, &f, 0);
    if (r != ESP_OK || f.len == 0) return r;
    if (f.len > 256) return ESP_FAIL;
    uint8_t b[257];
    f.payload = b;
    return httpd_ws_recv_frame(req, &f, f.len);
}

// --------------------------------------------------------------------------

void api_register_handlers(httpd_handle_t server) {
    s_srv = server;
    temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tc, &s_tsens) == ESP_OK) temperature_sensor_enable(s_tsens);
    else s_tsens = NULL;

    const httpd_uri_t uris[] = {
        { .uri = "/api/status",       .method = HTTP_GET,  .handler = h_status },
        { .uri = "/api/sensors",      .method = HTTP_GET,  .handler = h_sensors },
        { .uri = "/api/wifi/scan",    .method = HTTP_GET,  .handler = h_wifi_scan },
        { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = h_wifi_connect },
        { .uri = "/api/wifi/test",    .method = HTTP_GET,  .handler = h_wifi_test },
        { .uri = "/api/reboot",       .method = HTTP_POST, .handler = h_reboot },
        { .uri = "/api/room",         .method = HTTP_GET,  .handler = h_room_get },
        { .uri = "/api/room",         .method = HTTP_POST, .handler = h_room_set },
        { .uri = "/api/zones",        .method = HTTP_GET,  .handler = h_zones_get },
        { .uri = "/api/zones",        .method = HTTP_POST, .handler = h_zones_set },
        { .uri = "/api/radar",        .method = HTTP_POST, .handler = h_radar },
        { .uri = "/api/ota/github",   .method = HTTP_POST, .handler = h_ota_github },
        { .uri = "/api/ota/url",      .method = HTTP_POST, .handler = h_ota_url },
        { .uri = "/api/ota/upload",   .method = HTTP_POST, .handler = h_ota_upload },
        { .uri = "/api/ota/status",   .method = HTTP_GET,  .handler = h_ota_status },
        { .uri = "/api/update",       .method = HTTP_POST, .handler = h_update },
        { .uri = "/favicon.ico",      .method = HTTP_GET,  .handler = h_favicon },
        { .uri = "/ws",               .method = HTTP_GET,  .handler = h_ws, .is_websocket = true },
        // Na koncu: dopasowanie do wszystkiego (portal / 404)
        { .uri = "/*",                .method = HTTP_GET,  .handler = h_any },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(server, &uris[i]) != ESP_OK)
            ESP_LOGE(TAG, "Nie zarejestrowano %s", uris[i].uri);
    }
    xTaskCreate(ws_task, "ws_push", 3584, NULL, 5, NULL);
}
