#include "api_handlers.h"
#include "wifi_manager.h"
#include "nvs_config.h"
#include "ld2450.h"
#include "bme688.h"
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
#define JSON_CAP     1024

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

// Cialo zapytania POST jako JSON (maly limit - to tylko formularze panelu)
static cJSON *read_json(httpd_req_t *req) {
    if (req->content_len <= 0 || req->content_len > 512) return NULL;
    char buf[513];
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return NULL;
        }
        got += r;
    }
    buf[got] = 0;
    return cJSON_Parse(buf);
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
    int n = snprintf(buf, cap,
        "{\"up\":%lld,\"radar\":{\"online\":%s,\"fw\":\"%s\",\"fps\":%u,\"count\":%u,"
        "\"frames\":%lu,\"errors\":%lu,\"targets\":[",
        (long long)(esp_timer_get_time() / 1000), r.online ? "true" : "false", fw,
        r.fps, r.count, (unsigned long)r.frames, (unsigned long)r.errors);
    bool first = true;
    for (int i = 0; i < LD2450_MAX_TARGETS && n < cap; i++) {
        if (!r.t[i].valid) continue;
        n += snprintf(buf + n, cap - n, "%s{\"id\":%d,\"x\":%d,\"y\":%d,\"v\":%d,\"res\":%u}",
                      first ? "" : ",", i + 1, r.t[i].x_mm, r.t[i].y_mm, r.t[i].speed_cms,
                      r.t[i].res_mm);
        first = false;
    }
    if (n < cap) {
        n += snprintf(buf + n, cap - n,
            "]},\"bme\":{\"online\":%s,\"chip\":\"%s\",\"addr\":%u,\"t\":%.2f,\"h\":%.1f,"
            "\"p\":%.1f,\"gas\":%.1f,\"gas_ok\":%s,\"n\":%lu}}",
            b.online ? "true" : "false", b.chip, b.addr, b.temp_c, b.hum_pct, b.press_hpa,
            b.gas_kohm, b.gas_valid ? "true" : "false", (unsigned long)b.samples);
    }
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
    char buf[700];
    snprintf(buf, sizeof(buf),
        "{\"fw\":\"%s\",\"build\":\"%s %s\",\"idf\":\"%s\",\"uptime\":%lld,"
        "\"heap\":%lu,\"heap_min\":%lu,\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"chip_temp\":%.1f,"
        "\"wifi\":{\"mode\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"ch\":%d,"
        "\"host\":\"%s\",\"ap_ssid\":\"%s\",\"ap_clients\":%d}}",
        app->version, app->date, app->time, app->idf_ver,
        (long long)(esp_timer_get_time() / 1000000),
        (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size(),
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], chip_temp(),
        mode, ssid, ip, wifi_manager_get_rssi(), wifi_manager_get_channel(),
        wifi_manager_hostname(), wifi_manager_ap_ssid(), wifi_manager_ap_clients());
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

static void ws_send_work(void *arg) {
    ws_job_t *j = (ws_job_t *)arg;
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
        int fds[CONFIG_LWIP_MAX_SOCKETS];
        size_t n = CONFIG_LWIP_MAX_SOCKETS;
        if (httpd_get_client_list(s_srv, &n, fds) != ESP_OK) continue;
        int len = -1;
        for (size_t i = 0; i < n; i++) {
            if (httpd_ws_get_fd_info(s_srv, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
            if (len < 0) len = sensors_json(buf, JSON_CAP);
            ws_job_t *j = malloc(sizeof(ws_job_t) + len + 1);
            if (!j) break;
            j->fd = fds[i];
            j->len = len;
            memcpy(j->data, buf, len + 1);
            if (httpd_queue_work(s_srv, ws_send_work, j) != ESP_OK) free(j);
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
