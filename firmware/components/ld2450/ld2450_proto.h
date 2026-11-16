#pragma once
#include <stdint.h>
#include <stdbool.h>

// Protokol HLK-LD2450 (bez zaleznosci od ESP-IDF - testowany na komputerze).
//
// Ramka danych (30 B, ok. 10 na sekunde):
//   AA FF 03 00 | 3 x cel po 8 B | 55 CC
//   cel: X (int16), Y (int16), predkosc (int16), rozdzielczosc (uint16), LE.
//   X/Y/predkosc: najwyzszy bit 1 = wartosc dodatnia, 0 = ujemna (nie U2!).
//   X, Y i rozdzielczosc w mm, predkosc w cm/s.
//   Uklad: Y = odleglosc na wprost od radaru, X = w bok (patrzac jak radar).
// Ramka komendy/odpowiedzi:
//   FD FC FB FA | dlugosc (uint16 LE) | slowo komendy (uint16 LE) | dane | 04 03 02 01

#define LD2450_MAX_TARGETS 3
#define LD2450_DATA_LEN    30
#define LD2450_BUF_LEN     64

typedef struct {
    int16_t  x_mm;
    int16_t  y_mm;
    int16_t  speed_cms;   // + oddala sie, - zbliza
    uint16_t res_mm;      // rozdzielczosc odleglosci
    bool     valid;
} ld2450_target_t;

typedef enum {
    LD2450_FRAME_NONE = 0,
    LD2450_FRAME_DATA,
    LD2450_FRAME_ACK,
} ld2450_frame_t;

typedef struct {
    uint8_t  buf[LD2450_BUF_LEN];
    uint16_t len;     // bajty zebrane w biezacej ramce
    uint16_t need;    // dlugosc ramki odpowiedzi (z pola dlugosci)
    uint16_t flen;    // dlugosc ostatniej kompletnej ramki (dane w buf)
    uint8_t  kind;    // ld2450_frame_t biezacej ramki
} ld2450_parser_t;

void           ld2450_parser_reset(ld2450_parser_t *p);
// Podaje jeden bajt z UART. Gdy zwroci DATA lub ACK, cala ramka jest
// w p->buf (p->flen bajtow) do nastepnego wywolania.
ld2450_frame_t ld2450_parser_feed(ld2450_parser_t *p, uint8_t b);

// Ramka danych (30 B) -> 3 cele
void ld2450_decode_targets(const uint8_t *frame, ld2450_target_t out[LD2450_MAX_TARGETS]);

// Buduje komende, zwraca jej dlugosc.
int  ld2450_build_cmd(uint8_t *out, uint16_t cmd, const uint8_t *val, uint16_t vlen);

// Odpowiedz na komende 0x00A0 -> "V1.02.22062416". false gdy to inna ramka.
bool ld2450_parse_fw(const uint8_t *ack, int len, char *out, int cap);

#define LD2450_CMD_CONFIG_ON   0x00FF
#define LD2450_CMD_CONFIG_OFF  0x00FE
#define LD2450_CMD_READ_FW     0x00A0
