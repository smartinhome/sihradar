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
// Filtry celu
#define GATE_MM         800.0f     // dalej niz to (+ szybki krok) = inna osoba
#define GATE_PEN        1000.0f    // koszt osoby lub celu bez pary przy przypisywaniu
#define STILL_MOVE_MM   250.0f     // przesuniecie mediany w ~1 s = idzie
#define ALPHA_STILL     0.12f      // wygladzanie osoby nieruchomej (ok. 1 s)
#define ALPHA_MOVE      0.6f       // ...i idacej (opoznienie ok. 70 ms)
#define HOLD_MS         300u       // tyle przytrzymujemy cel zgubiony przez radar
#define CONFIRM_MS      400u       // tyle nowy cel musi byc widoczny bez przerwy
#define ZONE_HYST_MM    150.0f     // wyjscie ze strefy dopiero tyle za krawedzia

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
    memset(s->f, 0, sizeof(s->f));
}

void pz_zones_changed(pz_state_t *s) {
    memset(s->zone_tr, 0, sizeof(s->zone_tr));
    for (int i = 0; i < PZ_TARGETS; i++) s->f[i].zmask = 0;
}

static float med(const int16_t *v, int n) {
    int16_t t[PZ_MED_N];
    for (int i = 0; i < n; i++) {
        int16_t x = v[i];
        int j = i;
        while (j > 0 && t[j - 1] > x) { t[j] = t[j - 1]; j--; }
        t[j] = x;
    }
    return n % 2 ? t[n / 2] : (t[n / 2 - 1] + t[n / 2]) / 2.0f;
}

static int bits3(uint8_t m) { return (m & 1) + ((m >> 1) & 1) + ((m >> 2) & 1); }

static void filt_start(pz_filt_t *f, const pz_raw_t *r, uint32_t now) {
    memset(f, 0, sizeof(*f));
    f->on = true;
    f->x = r->x;
    f->y = r->y;
    f->speed = r->speed;
    f->res = r->res;
    f->first_ms = f->seen_ms = now;
    f->bx[0] = r->x; f->by[0] = r->y; f->bn = 1; f->bi = 1;
    f->hx[0] = r->x; f->hy[0] = r->y; f->hn = 1; f->hi = 1;
    f->moving = true;               // dopiero co sie pojawil
}

// Kolejny pomiar sledzonej osoby
static void filt_feed(pz_filt_t *f, const pz_raw_t *r, uint32_t now) {
    f->bx[f->bi] = r->x;
    f->by[f->bi] = r->y;
    f->bi = (f->bi + 1) % PZ_MED_N;
    if (f->bn < PZ_MED_N) f->bn++;
    float mx = med(f->bx, f->bn), my = med(f->by, f->bn);
    // ruch: radar mierzy predkosc w 2 z 3 ostatnich ramek albo mediana
    // przesunela sie wyraznie w ciagu ok. 1 s (ruch w bok ma predkosc ~0)
    bool fast = r->speed >= MOVE_SPEED_CMS || r->speed <= -MOVE_SPEED_CMS;
    f->vmask = (uint8_t)(((f->vmask << 1) | fast) & 7);
    int old = f->hn < PZ_HIST_N ? 0 : f->hi;
    float disp = hypotf(mx - f->hx[old], my - f->hy[old]);
    f->hx[f->hi] = mx;
    f->hy[f->hi] = my;
    f->hi = (f->hi + 1) % PZ_HIST_N;
    if (f->hn < PZ_HIST_N) f->hn++;
    f->dmask = (uint8_t)(((f->dmask << 1) | (disp > STILL_MOVE_MM)) & 7);
    // 2 z 3 ostatnich ramek - pojedynczy skok szumu nie przelacza w "idzie"
    f->moving = bits3(f->vmask) >= 2 || bits3(f->dmask) >= 2 || now - f->first_ms < TRANSIENT_MS;
    if (f->moving) {
        // idzie: biezacy pomiar, lekko wygladzony (mediana dawalaby opoznienie)
        f->x += ALPHA_MOVE * (r->x - f->x);
        f->y += ALPHA_MOVE * (r->y - f->y);
    } else {
        // siedzi / stoi: mediana odrzuca skoki z odbic, srednia tlumi drgania
        f->x += ALPHA_STILL * (mx - f->x);
        f->y += ALPHA_STILL * (my - f->y);
    }
    f->speed = r->speed;
    f->res = r->res;
    f->seen_ms = now;
    if (!f->confirmed && now - f->first_ms >= CONFIRM_MS) f->confirmed = true;
}

// Przypisanie celow radaru do sledzonych osob. Radar potrafi zamienic numery
// miedzy osobami (np. siedzaca przy biurku i druga dalej) - wtedy znaczniki
// i liczniki skakalyby miedzy nimi. Sprawdzamy wszystkie 6 par 3x3 i bierzemy
// najmniejsza laczna odleglosc; para dalsza niz bramka to dwie rozne osoby.
static void assoc(pz_state_t *s, const pz_raw_t raw[PZ_TARGETS], uint32_t now, int match[PZ_TARGETS]) {
    static const uint8_t P[6][3] = { {0,1,2}, {0,2,1}, {1,0,2}, {1,2,0}, {2,0,1}, {2,1,0} };
    float best = 1e30f;
    int bp = 0;
    uint8_t bm = 0;
    for (int p = 0; p < 6; p++) {
        float cost = 0;
        uint8_t m = 0;
        for (int t = 0; t < PZ_TARGETS; t++) {
            const pz_filt_t *f = &s->f[t];
            const pz_raw_t *r = &raw[P[p][t]];
            if (f->on && r->valid) {
                float d = hypotf(r->x - f->x, r->y - f->y);
                if (d <= GATE_MM + REACQ_MM_PER_MS * (float)(now - f->seen_ms)) {
                    cost += d;
                    m |= 1u << t;
                    continue;
                }
            }
            if (f->on) cost += GATE_PEN;
            if (r->valid) cost += GATE_PEN;
        }
        if (cost < best) { best = cost; bp = p; bm = m; }
    }
    for (int t = 0; t < PZ_TARGETS; t++) match[t] = (bm & (1u << t)) ? P[bp][t] : -1;
}

static void track_targets(pz_state_t *s, const pz_raw_t raw[PZ_TARGETS], uint32_t now, bool held[PZ_TARGETS]) {
    int match[PZ_TARGETS];
    bool used[PZ_TARGETS] = { false };
    assoc(s, raw, now, match);
    for (int t = 0; t < PZ_TARGETS; t++) {
        pz_filt_t *f = &s->f[t];
        held[t] = false;
        if (match[t] >= 0) {
            filt_feed(f, &raw[match[t]], now);
            used[match[t]] = true;
        } else if (f->on) {
            // Radar gubi osobe na 1-2 ramki - potwierdzona zostaje w miejscu.
            // Niepotwierdzona (pojedyncze ramki z odbic) przepada od razu.
            if (f->confirmed && now - f->seen_ms < HOLD_MS) {
                f->speed = 0;
                held[t] = true;
            } else {
                f->on = false;
            }
        }
    }
    // Nowe cele na wolne miejsca
    for (int r = 0; r < PZ_TARGETS; r++) {
        if (!raw[r].valid || used[r]) continue;
        for (int t = 0; t < PZ_TARGETS; t++)
            if (!s->f[t].on) { filt_start(&s->f[t], &raw[r], now); break; }
    }
}

// Strefa z histereza: wejscie na krawedzi, wyjscie dopiero ZONE_HYST_MM dalej
static bool in_zone_h(const pz_zone_t *z, float x, float y, bool was) {
    if (!was) return pz_in_zone(z, x, y);
    if (z->type == PZ_ZONE_OFF) return false;
    float m = ZONE_HYST_MM;
    float x1 = (z->x1 < z->x2 ? z->x1 : z->x2) - m, x2 = (z->x1 < z->x2 ? z->x2 : z->x1) + m;
    float y1 = (z->y1 < z->y2 ? z->y1 : z->y2) - m, y2 = (z->y1 < z->y2 ? z->y2 : z->y1) + m;
    return x >= x1 && x <= x2 && y >= y1 && y <= y2;
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
    bool held[PZ_TARGETS];
    track_targets(s, raw, now, held);

    for (int i = 0; i < PZ_TARGETS; i++) {
        pz_target_t *t = &s->t[i];
        pz_filt_t *f = &s->f[i];
        memset(t, 0, sizeof(*t));
        if (!f->on) continue;
        t->valid = true;
        t->held = held[i];
        t->moving = f->moving;
        t->rx = (int16_t)lroundf(f->x);
        t->ry = (int16_t)lroundf(f->y);
        t->speed = f->speed;
        t->res = f->res;
        t->dist = hypotf(f->x, f->y);
        pz_to_room(&s->room, f->x, f->y, &t->x, &t->y);
        // Strefy z histereza (pamietamy przynaleznosc z poprzedniej ramki)
        uint8_t zm = 0;
        for (int z = 0; z < PZ_MAX_ZONES; z++)
            if (in_zone_h(&s->zones[z], t->x, t->y, f->zmask & (1u << z))) zm |= 1u << z;
        f->zmask = zm;
        if (t->dist > s->cfg.max_dist_mm) { t->state = PZ_T_FAR; continue; }
        if (s->cfg.room_only && s->room.set && !in_room(&s->room, t->x, t->y)) {
            t->state = PZ_T_OUTSIDE;
            continue;
        }
        t->state = PZ_T_OK;
        for (int z = 0; z < PZ_MAX_ZONES; z++)
            if (s->zones[z].type == PZ_ZONE_IGNORE && (zm & (1u << z))) {
                t->state = PZ_T_IGNORED;
                break;
            }
        if (t->state != PZ_T_OK) continue;
        if (!f->confirmed) { t->state = PZ_T_PENDING; continue; }
        in_room_mask |= 1u << i;
        for (int z = 0; z < PZ_MAX_ZONES; z++)
            if (s->zones[z].type == PZ_ZONE_DETECT && (zm & (1u << z))) {
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
