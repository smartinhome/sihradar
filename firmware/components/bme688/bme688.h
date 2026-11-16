#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool    online;       // czujnik odpowiada i pomiar jest swiezy
    bool    gas_valid;    // pomiar gazu poprawny (grzalka osiagnela temperature)
    float   temp_c;
    float   hum_pct;
    float   press_hpa;
    float   gas_kohm;     // rezystancja warstwy gazowej (wieksza = czystsze powietrze)
    uint8_t addr;         // 0x76 / 0x77, 0 = nie znaleziono
    char    chip[8];      // "BME688" / "BME680"
    uint32_t samples;
} bme688_data_t;

// I2C + zadanie pomiarowe (pomiar co 3 s). Brak czujnika nie blokuje startu:
// zadanie szuka go co 10 s, wiec mozna go podlaczyc pozniej.
void bme688_init(int pin_sda, int pin_scl);
void bme688_get(bme688_data_t *out);
