#include "room.h"
#include "nvs_config.h"
#include "nvs.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ROOM";

// Osobny blob "room" w przestrzeni konfiguracji: reset sieci go nie rusza,
// a uklad sih_config_t zostaje bez zmian. Nowe pola tylko na koncu.
#define ROOM_KEY  "room"
#define ROOM_VER  1
#define MIN_MM    500
#define MAX_MM    20000

typedef struct {
    uint8_t  ver, flip, rsv0, rsv1;
    int32_t  w_mm, l_mm, x_mm, y_mm;
    int16_t  rot_deg, rsv2;
} room_blob_t;

static room_t s_room;

static void set_default(void) {
    // Pokoj 4 x 4 m, radar na srodku gornej sciany patrzy w glab
    memset(&s_room, 0, sizeof(s_room));
    s_room.w_mm = 4000;
    s_room.l_mm = 4000;
    s_room.x_mm = 2000;
    s_room.y_mm = 0;
}

void room_init(void) {
    set_default();
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    room_blob_t b;
    size_t sz = sizeof(b);
    if (nvs_get_blob(h, ROOM_KEY, &b, &sz) == ESP_OK && sz == sizeof(b) && b.ver == ROOM_VER) {
        s_room.set = true;
        s_room.w_mm = b.w_mm;
        s_room.l_mm = b.l_mm;
        s_room.x_mm = b.x_mm;
        s_room.y_mm = b.y_mm;
        s_room.rot_deg = b.rot_deg;
        s_room.flip = b.flip;
        ESP_LOGI(TAG, "Pokoj %ld x %ld mm, radar (%ld, %ld) mm, obrot %d st.",
                 (long)b.w_mm, (long)b.l_mm, (long)b.x_mm, (long)b.y_mm, b.rot_deg);
    }
    nvs_close(h);
}

room_t room_get(void) { return s_room; }

const char *room_save(const room_t *r) {
    if (r->w_mm < MIN_MM || r->w_mm > MAX_MM || r->l_mm < MIN_MM || r->l_mm > MAX_MM)
        return "Wymiary pokoju od 0,5 do 20 m.";
    if (r->x_mm < 0 || r->x_mm > r->w_mm || r->y_mm < 0 || r->y_mm > r->l_mm)
        return "Radar musi być w obrębie pokoju.";
    int rot = r->rot_deg % 360;
    if (rot > 180) rot -= 360;
    if (rot <= -180) rot += 360;
    room_blob_t b = {
        .ver = ROOM_VER, .flip = r->flip ? 1 : 0,
        .w_mm = r->w_mm, .l_mm = r->l_mm, .x_mm = r->x_mm, .y_mm = r->y_mm,
        .rot_deg = (int16_t)rot,
    };
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return "Nie udało się zapisać.";
    bool ok = nvs_set_blob(h, ROOM_KEY, &b, sizeof(b)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (!ok) return "Nie udało się zapisać.";
    s_room = *r;
    s_room.rot_deg = (int16_t)rot;
    s_room.set = true;
    ESP_LOGI(TAG, "Zapisano plan pokoju %ld x %ld mm", (long)r->w_mm, (long)r->l_mm);
    return NULL;
}

int room_json(char *buf, int cap) {
    return snprintf(buf, cap,
        "{\"set\":%s,\"w\":%ld,\"l\":%ld,\"x\":%ld,\"y\":%ld,\"rot\":%d,\"flip\":%s}",
        s_room.set ? "true" : "false", (long)s_room.w_mm, (long)s_room.l_mm,
        (long)s_room.x_mm, (long)s_room.y_mm, s_room.rot_deg, s_room.flip ? "true" : "false");
}
