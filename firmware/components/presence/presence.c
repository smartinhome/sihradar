#include "presence.h"
#include "ld2450.h"
#include "room.h"
#include "nvs_config.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "PRES";

// Osobne bloby, nowe pola tylko na koncu (jak w room.c)
#define CFG_KEY   "pzcfg"
#define ZONES_KEY "zones"
#define BLOB_VER  1
#define PERIOD_MS 100

typedef struct {
    uint8_t  ver, memory, room_only, rsv;
    uint16_t max_dist_mm, hold_s, zone_off_s, latch_min, exit_mm, rsv2;
} cfg_blob_t;

typedef struct {
    uint8_t   ver, n, rsv[2];
    pz_zone_t z[PZ_MAX_ZONES];
} zones_blob_t;

static pz_state_t        s_st;
static SemaphoreHandle_t s_mx;

static void lock(void)   { xSemaphoreTake(s_mx, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mx); }

static void load(void) {
    pz_cfg_default(&s_st.cfg);
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    cfg_blob_t c;
    size_t sz = sizeof(c);
    if (nvs_get_blob(h, CFG_KEY, &c, &sz) == ESP_OK && sz == sizeof(c) && c.ver == BLOB_VER) {
        pz_cfg_t v = {
            .max_dist_mm = c.max_dist_mm, .hold_s = c.hold_s, .zone_off_s = c.zone_off_s,
            .latch_min = c.latch_min, .exit_mm = c.exit_mm,
            .memory = c.memory, .room_only = c.room_only,
        };
        if (!pz_check_cfg(&v)) s_st.cfg = v;
    }
    static zones_blob_t zb;     // ~280 B - nie na stosie zadania main
    sz = sizeof(zb);
    if (nvs_get_blob(h, ZONES_KEY, &zb, &sz) == ESP_OK && sz == sizeof(zb) && zb.ver == BLOB_VER) {
        for (int i = 0; i < PZ_MAX_ZONES && i < zb.n; i++) {
            zb.z[i].name[PZ_NAME_LEN - 1] = 0;
            if (!pz_check_zone(&zb.z[i], NULL)) s_st.zones[i] = zb.z[i];
        }
        ESP_LOGI(TAG, "Strefy: %d", zb.n);
    }
    nvs_close(h);
}

static void sync_room(void) {
    room_t r = room_get();
    s_st.room = (pz_room_t){ .set = r.set, .w = r.w_mm, .l = r.l_mm, .x = r.x_mm, .y = r.y_mm,
                             .rot = r.rot_deg, .flip = r.flip };
}

static void task(void *arg) {
    (void)arg;
    ld2450_data_t d;
    TickType_t wake = xTaskGetTickCount();
    while (1) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(PERIOD_MS));
        ld2450_get(&d);
        pz_raw_t raw[PZ_TARGETS];
        for (int i = 0; i < PZ_TARGETS; i++)
            raw[i] = (pz_raw_t){ d.t[i].valid, d.t[i].x_mm, d.t[i].y_mm, d.t[i].speed_cms };
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        lock();
        sync_room();       // plan moze sie zmienic w panelu
        bool was = s_st.room_tr.occ;
        pz_update(&s_st, raw, d.online, now);
        bool is = s_st.room_tr.occ;
        unlock();
        if (was != is) ESP_LOGI(TAG, "Pokoj: %s", is ? "obecnosc" : "pusto");
    }
}

void presence_init(void) {
    s_mx = xSemaphoreCreateMutex();
    memset(&s_st, 0, sizeof(s_st));
    load();
    sync_room();
    xTaskCreate(task, "presence", 3072, NULL, 5, NULL);
}

pz_cfg_t presence_get_cfg(void) {
    lock();
    pz_cfg_t c = s_st.cfg;
    unlock();
    return c;
}

int presence_json(char *buf, int cap) {
    lock();
    const pz_state_t *s = &s_st;
    int n = snprintf(buf, cap, "{\"occ\":%s,\"latch\":%s,\"n\":%u,\"held\":%u,\"z\":[",
                     s->room_tr.occ ? "true" : "false", s->room_tr.latch ? "true" : "false",
                     s->room_tr.n, s->room_tr.held);
    bool first = true;
    for (int z = 0; z < PZ_MAX_ZONES && n < cap; z++) {
        if (s->zones[z].type == PZ_ZONE_OFF) continue;
        const pz_track_t *t = &s->zone_tr[z];
        n += snprintf(buf + n, cap - n, "%s[%d,%d,%u]", first ? "" : ",", t->occ, t->latch, t->n);
        first = false;
    }
    if (n < cap)
        n += snprintf(buf + n, cap - n, "],\"ts\":[%u,%u,%u],\"tz\":[%u,%u,%u]}",
                      s->t[0].state, s->t[1].state, s->t[2].state,
                      s->t[0].zones, s->t[1].zones, s->t[2].zones);
    unlock();
    return n < cap ? n : cap - 1;
}

static void esc(char *out, size_t cap, const char *s) {
    size_t e = 0;
    for (size_t k = 0; s[k] && e + 2 < cap; k++) {
        unsigned char c = (unsigned char)s[k];
        if (c < 0x20) continue;
        if (c == '"' || c == '\\') out[e++] = '\\';
        out[e++] = (char)c;
    }
    out[e] = 0;
}

int presence_zones_json(char *buf, int cap) {
    lock();
    const pz_cfg_t *c = &s_st.cfg;
    int n = snprintf(buf, cap,
        "{\"cfg\":{\"dist\":%u,\"hold\":%u,\"zoff\":%u,\"latch\":%u,\"exit\":%u,\"mem\":%s,\"room\":%s},\"zones\":[",
        c->max_dist_mm, c->hold_s, c->zone_off_s, c->latch_min, c->exit_mm,
        c->memory ? "true" : "false", c->room_only ? "true" : "false");
    bool first = true;
    for (int z = 0; z < PZ_MAX_ZONES && n < cap; z++) {
        const pz_zone_t *zz = &s_st.zones[z];
        if (zz->type == PZ_ZONE_OFF) continue;
        char nm[PZ_NAME_LEN * 2];
        esc(nm, sizeof(nm), zz->name);
        n += snprintf(buf + n, cap - n, "%s{\"name\":\"%s\",\"t\":%u,\"x1\":%d,\"y1\":%d,\"x2\":%d,\"y2\":%d}",
                      first ? "" : ",", nm, zz->type, zz->x1, zz->y1, zz->x2, zz->y2);
        first = false;
    }
    if (n < cap) n += snprintf(buf + n, cap - n, "]}");
    unlock();
    return n < cap ? n : cap - 1;
}

const char *presence_save(const pz_cfg_t *cfg, const pz_zone_t *zones, int n) {
    if (n < 0 || n > PZ_MAX_ZONES) return "Najwyżej 8 stref.";
    const char *err = pz_check_cfg(cfg);
    if (err) return err;
    static zones_blob_t zb;
    memset(&zb, 0, sizeof(zb));
    zb.ver = BLOB_VER;
    zb.n = (uint8_t)n;
    for (int i = 0; i < n; i++) {
        if ((err = pz_check_zone(&zones[i], NULL))) return err;
        zb.z[i] = zones[i];
        zb.z[i].name[PZ_NAME_LEN - 1] = 0;
    }
    cfg_blob_t cb = {
        .ver = BLOB_VER, .memory = cfg->memory, .room_only = cfg->room_only,
        .max_dist_mm = cfg->max_dist_mm, .hold_s = cfg->hold_s, .zone_off_s = cfg->zone_off_s,
        .latch_min = cfg->latch_min, .exit_mm = cfg->exit_mm,
    };
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return "Nie udało się zapisać.";
    bool ok = nvs_set_blob(h, CFG_KEY, &cb, sizeof(cb)) == ESP_OK &&
              nvs_set_blob(h, ZONES_KEY, &zb, sizeof(zb)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (!ok) return "Nie udało się zapisać.";

    lock();
    bool zones_changed = memcmp(s_st.zones, zb.z, sizeof(s_st.zones)) != 0;
    s_st.cfg = *cfg;
    memcpy(s_st.zones, zb.z, sizeof(s_st.zones));
    if (zones_changed) memset(s_st.zone_tr, 0, sizeof(s_st.zone_tr));   // nowe obszary - od zera
    unlock();
    ESP_LOGI(TAG, "Zapisano %d stref, zasieg %u mm", n, cfg->max_dist_mm);
    return NULL;
}
