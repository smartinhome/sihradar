#include "webserver.h"
#include "api_handlers.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "WEBSERVER";
static httpd_handle_t s_server = NULL;

// Panel (webui/index.html) wbudowany jako blob gzip - patrz CMakeLists.txt.
// Przegladarka rozpakowuje go sama (Content-Encoding: gzip).
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    // no-cache: przegladarka trzyma kopie, ale pyta serwer przed uzyciem -
    // po aktualizacji firmware panel odswiezy sie bez Ctrl+F5.
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)index_html_gz_start,
                           index_html_gz_end - index_html_gz_start);
}

void webserver_init(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.stack_size       = 8192;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 7;
    // Uspiony telefon z otwartym WebSocketem nie moze blokowac serwera na 5 s
    cfg.send_wait_timeout = 2;
    // Dowolny adres (portal: telefon pyta o /generate_204, /hotspot-detect.html...)
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Nie udalo sie uruchomic serwera HTTP");
        return;
    }
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    httpd_register_uri_handler(s_server, &root);
    api_register_handlers(s_server);
    ESP_LOGI(TAG, "Serwer HTTP na porcie 80 (panel %u B gz)",
             (unsigned)(index_html_gz_end - index_html_gz_start));
}
