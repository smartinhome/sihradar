#pragma once
#include <stdint.h>
#include <stdbool.h>

// Kompensacja odczytow BME680/BME688 wg wzorow zmiennoprzecinkowych Bosch
// (BME68x Sensor API). Bez zaleznosci od ESP-IDF - testowana na komputerze.

#define BME68X_COEFF1_ADDR 0x8A
#define BME68X_COEFF1_LEN  23
#define BME68X_COEFF2_ADDR 0xE1
#define BME68X_COEFF2_LEN  14
#define BME68X_COEFF3_ADDR 0x00
#define BME68X_COEFF3_LEN  5
#define BME68X_COEFF_LEN   (BME68X_COEFF1_LEN + BME68X_COEFF2_LEN + BME68X_COEFF3_LEN)

typedef struct {
    uint16_t par_t1; int16_t par_t2; int8_t par_t3;
    uint16_t par_p1; int16_t par_p2; int8_t par_p3; int16_t par_p4; int16_t par_p5;
    int8_t par_p6; int8_t par_p7; int16_t par_p8; int16_t par_p9; uint8_t par_p10;
    uint16_t par_h1; uint16_t par_h2; int8_t par_h3; int8_t par_h4; int8_t par_h5;
    uint8_t par_h6; int8_t par_h7;
    int8_t par_gh1; int16_t par_gh2; int8_t par_gh3;
    uint8_t res_heat_range; int8_t res_heat_val; int8_t range_sw_err;
    float t_fine;
} bme68x_calib_t;

// coeff: 0x8A..0xA0 (23 B), 0xE1..0xEE (14 B), 0x00..0x04 (5 B) - jeden za drugim
void    bme68x_parse_calib(const uint8_t coeff[BME68X_COEFF_LEN], bme68x_calib_t *c);

// Kolejnosc wywolan: najpierw temperatura (ustawia t_fine), potem reszta.
float   bme68x_temperature(bme68x_calib_t *c, uint32_t adc);      // °C
float   bme68x_pressure(const bme68x_calib_t *c, uint32_t adc);   // Pa
float   bme68x_humidity(const bme68x_calib_t *c, uint16_t adc);   // %
// BME688 (wariant "gas high") i BME680 (wariant "gas low") licza gaz inaczej
float   bme68x_gas_high(uint16_t adc, uint8_t range);             // Ohm
float   bme68x_gas_low(const bme68x_calib_t *c, uint16_t adc, uint8_t range);

// Rejestry grzalki
uint8_t bme68x_heater_res(const bme68x_calib_t *c, uint16_t target_c, int8_t amb_c);
uint8_t bme68x_gas_wait(uint16_t ms);
