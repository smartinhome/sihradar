#pragma once
#include <stdbool.h>
#include <stdint.h>

// Plan pomieszczenia i polozenie radaru - rysowane w panelu, zapisane w module,
// zeby kazde urzadzenie (telefon, komputer) widzialo ten sam plan. Tu trafia
// pozniej takze lista stref detekcji.
//
// Uklad wspolrzednych pokoju (mm): poczatek w lewym gornym rogu planu,
// x w prawo (szerokosc), y w dol (dlugosc).
// Obrot radaru (stopnie): 0 = radar patrzy w dol planu (od gornej sciany
// w glab pokoju), dodatnie zgodnie z ruchem wskazowek zegara na planie.
typedef struct {
    bool    set;        // plan zapisany przez uzytkownika
    int32_t w_mm;       // szerokosc (sciana gorna/dolna)
    int32_t l_mm;       // dlugosc (sciana lewa/prawa)
    int32_t x_mm;       // polozenie radaru
    int32_t y_mm;
    int16_t rot_deg;    // -179..180
    bool    flip;       // radar zamontowany "do gory nogami" - odwrocona os X
} room_t;

void        room_init(void);
room_t      room_get(void);
// NULL = zapisano, inaczej komunikat dla panelu
const char *room_save(const room_t *r);
int         room_json(char *buf, int cap);
