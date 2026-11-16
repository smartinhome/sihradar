#include "presence_core.h"
#include <math.h>
#include <string.h>

#define DEG             (3.14159265f / 180.0f)
#define ROOM_TOL_MM     150.0f     // radar myli sie o kilka-kilkanascie cm przy scianach
#define FOV_DEG         60.0f      // pole widzenia LD2450: +-60 st.
#define ANCHOR_MS       600u       // co tyle odswiezamy punkt kontrolny ruchu
#define TRANSIENT_MS    1200u      // tuz po pojawieniu sie osoba "w ruchu"
#define MOVE_DISP_MM    180.0f     // przesuniecie miedzy punktami = ruch
#define MOVE_SPEED_CMS  15         // predkosc z radaru = ruch
#define REACQ_MS        3000u      // tyle szukamy znikniętej osoby obok
#define REACQ_BASE_MM   600.0f     // + szum pomiaru
#define REACQ_MM_PER_MS 1.6f       // + szybki krok 1,6 m/s

void pz_cfg_default(pz_cfg_t *c) {
    c->max_dist_mm = 6000;
    c->hold_s = 30;
    c->zone_off_s = 5;
    c->latch_min = 30;
    c->exit_mm = 400;
    c->memory = true;
    c->room_only = true;
}

void pz_reset(pz_state_t *s) {
    memset(&s->room_tr, 0, sizeof(s->room_tr));
    memset(s->zone_tr, 0, sizeof(s->zone_tr));
    memset(s->t, 0, sizeof(s->t));
}

void pz_to_room(const pz_room_t *r, float tx, float ty, float *x, float *y) {
    // Jak toRoom() w panelu: f = kierunek patrzenia, rr = prawa strona radaru
    float t = r->rot * DEG;
    float fx = -sinf(t), fy = cosf(t);
    float rx = -cosf(t), ry = -sinf(t);
    float X = r->flip ? -tx : tx;
    *x = r->x + X * rx + ty * fx;
    *y = r->y + X * ry + ty * fy;
}

bool pz_in_zone(const pz_zone_t *z, float x, float y) {
    if (z->type == PZ_ZONE_OFF) return false;
    float x1 = z->x1 < z->x2 ? z->x1 : z->x2, x2 = z->x1 < z->x2 ? z->x2 : z->x1;
    float y1 = z->y1 < z->y2 ? z->y1 : z->y2, y2 = z->y1 < z->y2 ? z->y2 : z->y1;
    return x >= x1 && x <= x2 && y >= y1 && y <= y2;
}

static bool in_room(const pz_room_t *r, float x, float y) {
    return x >= -ROOM_TOL_MM && x <= r->w + ROOM_TOL_MM && y >= -ROOM_TOL_MM && y <= r->l + ROOM_TOL_MM;
}

// Czy z tego miejsca mozna "zniknac", wychodzac: sciana (drzwi), granica
// zasiegu albo bok pola widzenia radaru.
static bool near_exit(const pz_state_t *s, const pz_occ_t *o) {
    float m = s->cfg.exit_mm;
    if (o->dist >= s->cfg.max_dist_mm - m) return true;
    if (s->room.set && (o->x <= m || s->room.w - o->x <= m || o->y <= m || s->room.l - o->y <= m))
        return true;
    // odleglosc od krawedzi stozka +-60 st.
    float dx = o->x - s->room.x, dy = o->y - s->room.y;
    float t = s->room.rot * DEG;
    float fwd = dx * -sinf(t) + dy * cosf(t);
    float side = fabsf(dx * -cosf(t) + dy * -sinf(t));
    float ang = atan2f(side, fwd);
    float edge = o->dist * sinf(FOV_DEG * DEG - ang);
    return ang >= FOV_DEG * DEG || edge <= m;
}

static void occ_update(pz_occ_t *o, const pz_target_t *t, uint32_t now) {
    if (!o->used) {
        memset(o, 0, sizeof(*o));
        o->used = true;
        o->first_ms = o->anchor_ms = now;
        o->ax = o->px = t->x;
        o->ay = o->py = t->y;
    }
    if (now - o->anchor_ms >= ANCHOR_MS) {
        o->px = o->ax; o->py = o->ay;
        o->ax = t->x;  o->ay = t->y;
        o->anchor_ms = now;
    }
    float disp = hypotf(t->x - o->px, t->y - o->py);
    o->moving = (t->speed >= MOVE_SPEED_CMS || t->speed <= -MOVE_SPEED_CMS) ||
                disp >= MOVE_DISP_MM || now - o->first_ms < TRANSIENT_MS;
    o->x = t->x;
    o->y = t->y;
    o->dist = t->dist;
    o->seen_ms = now;
}

// Czy cel widoczny poza obszarem to ta sama osoba, ktora z niego zniknela
static bool same_person(const pz_occ_t *o, const pz_target_t *t, uint32_t now) {
    uint32_t el = now - o->seen_ms;
    if (el > REACQ_MS) return false;
    return hypotf(t->x - o->x, t->y - o->y) <= REACQ_BASE_MM + REACQ_MM_PER_MS * (float)el;
}

// Wspolny automat dla pokoju i stref. inside: bity celow w obszarze.
static void track(pz_state_t *s, pz_track_t *tr, uint8_t inside, uint32_t off_ms, uint32_t now) {
    uint8_t held = 0;
    if (inside) {
        for (int i = 0; i < PZ_TARGETS; i++) {
            pz_occ_t *o = &tr->o[i];
            if (inside & (1u << i)) { occ_update(o, &s->t[i], now); continue; }
            // Slot poza obszarem: jesli radar stracil ten cel (a nie wyszedl
            // nim gdzies dalej), osoba mogla zostac nieruchomo - zapis zostaje
            if (o->used && (s->t[i].valid || o->moving)) o->used = false;
            // ...chyba ze pojawil sie znowu obok (radar dal mu inny numer)
            for (int j = 0; j < PZ_TARGETS && o->used; j++)
                if (s->t[j].valid && hypotf(s->t[j].x - o->x, s->t[j].y - o->y) <= 1000.0f)
                    o->used = false;
            if (o->used) held++;
        }
        tr->occ = true;
        tr->latch = false;
        tr->last_ms = now;
        tr->n = 0;
        for (int i = 0; i < PZ_TARGETS; i++) if (inside & (1u << i)) tr->n++;
        tr->held = held;
        return;
    }

    // Nikogo nie widac w obszarze - kto z zapamietanych wyszedl?
    tr->n = 0;
    for (int i = 0; i < PZ_TARGETS; i++) {
        pz_occ_t *o = &tr->o[i];
        if (!o->used) continue;
        bool gone = false;
        for (int j = 0; j < PZ_TARGETS && !gone; j++)
            if (s->t[j].valid && same_person(o, &s->t[j], now)) gone = true;
        if (!gone && o->moving && near_exit(s, o)) gone = true;
        if (gone) o->used = false;
        else held++;
    }
    tr->held = held;
    uint32_t since = now - tr->last_ms;
    if (!held || !s->cfg.memory) {
        tr->latch = false;
        if (since >= off_ms) {
            tr->occ = false;
            tr->held = 0;
            memset(tr->o, 0, sizeof(tr->o));
        }
    } else if (since >= (uint32_t)s->cfg.latch_min * 60000u) {
        tr->occ = tr->latch = false;
        tr->held = 0;
        memset(tr->o, 0, sizeof(tr->o));
    } else {
        tr->occ = true;
        tr->latch = true;
    }
}

void pz_update(pz_state_t *s, const pz_raw_t raw[PZ_TARGETS], bool online, uint32_t now) {
    if (!online) return;
    uint8_t in_room_mask = 0;
    uint8_t in_zone[PZ_MAX_ZONES] = {0};

    for (int i = 0; i < PZ_TARGETS; i++) {
        pz_target_t *t = &s->t[i];
        memset(t, 0, sizeof(*t));
        if (!raw[i].valid) continue;
        t->valid = true;
        t->rx = raw[i].x;
        t->ry = raw[i].y;
        t->speed = raw[i].speed;
        t->dist = hypotf(raw[i].x, raw[i].y);
        pz_to_room(&s->room, raw[i].x, raw[i].y, &t->x, &t->y);
        if (t->dist > s->cfg.max_dist_mm) { t->state = PZ_T_FAR; continue; }
        if (s->cfg.room_only && s->room.set && !in_room(&s->room, t->x, t->y)) {
            t->state = PZ_T_OUTSIDE;
            continue;
        }
        t->state = PZ_T_OK;
        for (int z = 0; z < PZ_MAX_ZONES; z++)
            if (s->zones[z].type == PZ_ZONE_IGNORE && pz_in_zone(&s->zones[z], t->x, t->y)) {
                t->state = PZ_T_IGNORED;
                break;
            }
        if (t->state != PZ_T_OK) continue;
        in_room_mask |= 1u << i;
        for (int z = 0; z < PZ_MAX_ZONES; z++)
            if (s->zones[z].type == PZ_ZONE_DETECT && pz_in_zone(&s->zones[z], t->x, t->y)) {
                t->zones |= 1u << z;
                in_zone[z] |= 1u << i;
            }
    }

    bool zone_latched = false;
    for (int z = 0; z < PZ_MAX_ZONES; z++) {
        pz_track_t *tr = &s->zone_tr[z];
        if (s->zones[z].type != PZ_ZONE_DETECT) { memset(tr, 0, sizeof(*tr)); continue; }
        track(s, tr, in_zone[z], (uint32_t)s->cfg.zone_off_s * 1000u, now);
        if (tr->latch) zone_latched = true;
    }
    track(s, &s->room_tr, in_room_mask, (uint32_t)s->cfg.hold_s * 1000u, now);
    // Ktos zapamietany w strefie = pokoj tez zajety
    if (zone_latched && !s->room_tr.occ) {
        s->room_tr.occ = true;
        s->room_tr.latch = true;
    }
}

const char *pz_check_cfg(const pz_cfg_t *c) {
    if (c->max_dist_mm < 500 || c->max_dist_mm > 7500) return "Zasięg od 0,5 do 7,5 m.";
    if (c->hold_s > 3600 || c->zone_off_s > 3600) return "Opóźnienie najwyżej 3600 s.";
    if (c->latch_min < 1 || c->latch_min > 240) return "Pamięć od 1 do 240 min.";
    if (c->exit_mm < 100 || c->exit_mm > 1500) return "Margines wyjścia od 0,1 do 1,5 m.";
    return NULL;
}

const char *pz_check_zone(const pz_zone_t *z, const pz_room_t *r) {
    (void)r;   // strefa moze wystawac za pokoj (np. po zmniejszeniu planu)
    if (z->type != PZ_ZONE_DETECT && z->type != PZ_ZONE_IGNORE) return "Nieznany rodzaj strefy.";
    if (z->x1 < 0 || z->y1 < 0 || z->x2 < 0 || z->y2 < 0 ||
        z->x1 > 20000 || z->y1 > 20000 || z->x2 > 20000 || z->y2 > 20000)
        return "Strefa poza planem.";
    int dx = z->x2 - z->x1, dy = z->y2 - z->y1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx < 200 || dy < 200) return "Strefa musi mieć co najmniej 20 × 20 cm.";
    return NULL;
}
