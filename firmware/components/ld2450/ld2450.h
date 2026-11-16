#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "ld2450_proto.h"

typedef struct {
    bool            online;      // ramka danych w ostatniej sekundzie
    uint8_t         count;       // ilu celow wykryto
    ld2450_target_t t[LD2450_MAX_TARGETS];
    uint32_t        frames;      // poprawnych ramek od startu
    uint32_t        errors;      // bajtow odrzuconych przez parser (szum na linii)
    uint8_t         fps;         // ramek w ostatniej sekundzie
    char            fw[24];      // wersja firmware radaru ("" = nieznana)
} ld2450_data_t;

// UART + zadanie odbioru. Radar pracuje na 256000 bd.
void ld2450_init(int uart_num, int pin_tx, int pin_rx);
void ld2450_get(ld2450_data_t *out);
