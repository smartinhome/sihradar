#include "nvs_config.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "CFG";
static sih_config_t g_cfg;

static void set_defaults(sih_config_t *c) {
    memset(c, 0, sizeof(*c));
    strlcpy(c->ap_pass, "smartinhome", sizeof(c->ap_pass));
    c->upd_check = true;
}

void nvs_config_init(void) {
    set_defaults(&g_cfg);
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Brak konfiguracji w NVS, uzywam domyslnej");
        return;
    }
    // Starszy (krotszy) blob wczytujemy w calosci, nowe pola zostaja domyslne.
    size_t stored = 0;
    if (nvs_get_blob(h, "config", NULL, &stored) == ESP_OK &&
        stored > 0 && stored <= sizeof(g_cfg)) {
        sih_config_t c;
        set_defaults(&c);
        if (nvs_get_blob(h, "config", &c, &stored) == ESP_OK) g_cfg = c;
    }
    nvs_close(h);
    // Zabezpieczenie przed uszkodzonym blobem: napisy zawsze zakonczone zerem
    g_cfg.wifi_ssid[sizeof(g_cfg.wifi_ssid) - 1] = 0;
    g_cfg.wifi_pass[sizeof(g_cfg.wifi_pass) - 1] = 0;
    g_cfg.ap_pass[sizeof(g_cfg.ap_pass) - 1] = 0;
    if (strlen(g_cfg.ap_pass) < 8) strlcpy(g_cfg.ap_pass, "smartinhome", sizeof(g_cfg.ap_pass));
    ESP_LOGI(TAG, "Konfiguracja wczytana, siec: %s",
             g_cfg.wifi_ssid[0] ? g_cfg.wifi_ssid : "(brak)");
}

sih_config_t nvs_config_get(void) { return g_cfg; }

const sih_config_t *nvs_config_ptr(void) { return &g_cfg; }

bool nvs_config_save(const sih_config_t *cfg) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, "config", cfg, sizeof(*cfg)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) g_cfg = *cfg;
    else ESP_LOGE(TAG, "Zapis konfiguracji nieudany");
    return ok;
}

void nvs_config_forget_wifi(void) {
    sih_config_t c = g_cfg;
    memset(c.wifi_ssid, 0, sizeof(c.wifi_ssid));
    memset(c.wifi_pass, 0, sizeof(c.wifi_pass));
    nvs_config_save(&c);
}
