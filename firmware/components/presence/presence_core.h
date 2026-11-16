#pragma once
#include <stdbool.h>
#include <stdint.h>

// Logika obecnosci (bez zaleznosci od ESP-IDF - testowana na komputerze).
//
// Pomysl na wzor esphome-radar-suite (bb12ett), napisany od nowa dla SIH Radar:
// - cele radaru przeliczane na wspolrzedne pokoju (plan z zakladki Pomieszczenie),
// - odrzucanie celow za daleko, poza obrysem pokoju i w strefach "pomijaj"
//   (wentylator, firanka, odbicie w oknie),
// - strefy obecnosci (prostokaty na planie) z liczba osob,
// - "pamiec" osoby nieruchomej: radar przestaje widziec kogos, kto siedzi
//   lub lezy bez ruchu. Jesli zniknal w srodku pokoju, nie idac w strone
//   wyjscia, uznajemy ze nadal tam jest - do czasu az ruszy sie albo minie
//   limit pamieci. Zniknal przy scianie / na granicy zasiegu w ruchu albo
//   pojawil sie obok poza strefa - wyszedl.
//
// Uklad pokoju jak w room.h: mm, (0,0) lewy gorny rog planu, x w prawo, y w dol.

#define PZ_TARGETS   3
#define PZ_MAX_ZONES 8
#define PZ_NAME_LEN  24      // bajtow z koncowym zerem (UTF-8)

typedef enum {
    PZ_ZONE_OFF = 0,
    PZ_ZONE_DETECT = 1,      // strefa obecnosci
    PZ_ZONE_IGNORE = 2,      // strefa pomijana (cele w niej nie istnieja)
} pz_zone_type_t;

typedef struct {
    int16_t x1, y1, x2, y2;  // przeciwlegle rogi, mm na planie
    uint8_t type;            // pz_zone_type_t
    char    name[PZ_NAME_LEN];
} pz_zone_t;

typedef struct {
    uint16_t max_dist_mm;    // dalsze cele pomijane (odbicia za scianami)
    uint16_t hold_s;         // obecnosc w pokoju trwa jeszcze tyle po ostatnim ruchu
    uint16_t zone_off_s;     // to samo dla stref
    uint16_t latch_min;      // limit pamieci osoby nieruchomej
    uint16_t exit_mm;        // tak blisko sciany / granicy zasiegu = wyszedl
    bool     memory;         // pamiec osoby nieruchomej
    bool     room_only;      // licz tylko osoby w obrysie pokoju (gdy plan zapisany)
} pz_cfg_t;

// Geometria pokoju i radaru (kopia potrzebnych pol room_t)
typedef struct {
    bool    set;
    int32_t w, l, x, y;
    int16_t rot;
    bool    flip;
} pz_room_t;

// Cel radaru po przeliczeniu
typedef enum {
    PZ_T_NONE = 0,           // brak celu
    PZ_T_OK,                 // liczony
    PZ_T_FAR,                // dalej niz max_dist
    PZ_T_OUTSIDE,            // poza obrysem pokoju
    PZ_T_IGNORED,            // w strefie pomijanej
} pz_tstate_t;

typedef struct {
    bool    valid;           // radar go widzi
    int16_t rx, ry;          // wspolrzedne radaru (mm)
    int16_t speed;           // cm/s
    float   x, y;            // na planie (mm)
    float   dist;            // od radaru (mm)
    uint8_t state;           // pz_tstate_t
    uint8_t zones;           // bity stref obecnosci, w ktorych jest
} pz_target_t;

// Zapis o osobie w obszarze (na kazdy z 3 slotow radaru)
typedef struct {
    bool     used;
    bool     moving;
    float    x, y, dist;
    float    ax, ay, px, py; // punkt kontrolny (biezacy i poprzedni)
    uint32_t anchor_ms, first_ms, seen_ms;
} pz_occ_t;

typedef struct {
    bool     occ;            // ktos jest
    bool     latch;          // ...ale tylko z pamieci (nikt nie jest widoczny)
    uint8_t  n;              // ile osob widac teraz
    uint8_t  held;           // ile osob zapamietanych
    uint32_t last_ms;        // ostatnio ktos widoczny
    pz_occ_t o[PZ_TARGETS];
} pz_track_t;

typedef struct {
    pz_cfg_t    cfg;
    pz_room_t   room;
    pz_zone_t   zones[PZ_MAX_ZONES];
    pz_target_t t[PZ_TARGETS];
    pz_track_t  room_tr;
    pz_track_t  zone_tr[PZ_MAX_ZONES];
} pz_state_t;

void pz_cfg_default(pz_cfg_t *c);
// Zeruje stan trackerow (np. po zmianie stref)
void pz_reset(pz_state_t *s);

// Cel radaru (X w bok, Y na wprost) -> plan pokoju
void pz_to_room(const pz_room_t *r, float tx, float ty, float *x, float *y);
bool pz_in_zone(const pz_zone_t *z, float x, float y);

// Jedna ramka radaru. raw: 3 cele (valid=false gdy brak), now_ms rosnacy zegar.
// online=false: radar milczy - zachowujemy stan (nie kasujemy obecnosci
// z powodu przerwy w lacznosci).
typedef struct { bool valid; int16_t x, y, speed; } pz_raw_t;
void pz_update(pz_state_t *s, const pz_raw_t raw[PZ_TARGETS], bool online, uint32_t now_ms);

// Walidacja przed zapisem; NULL = ok, inaczej komunikat dla panelu
const char *pz_check_cfg(const pz_cfg_t *c);
const char *pz_check_zone(const pz_zone_t *z, const pz_room_t *r);
