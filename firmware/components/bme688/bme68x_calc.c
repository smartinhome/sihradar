#include "bme68x_calc.h"

// Indeksy w tablicy wspolczynnikow (jak BME68X_IDX_* w bme68x_defs.h)
enum {
    I_T2_LSB = 0, I_T2_MSB = 1, I_T3 = 2,
    I_P1_LSB = 4, I_P1_MSB = 5, I_P2_LSB = 6, I_P2_MSB = 7, I_P3 = 8,
    I_P4_LSB = 10, I_P4_MSB = 11, I_P5_LSB = 12, I_P5_MSB = 13, I_P7 = 14, I_P6 = 15,
    I_P8_LSB = 18, I_P8_MSB = 19, I_P9_LSB = 20, I_P9_MSB = 21, I_P10 = 22,
    I_H2_MSB = 23, I_H2_LSB = 24, I_H1_LSB = 24, I_H1_MSB = 25,
    I_H3 = 26, I_H4 = 27, I_H5 = 28, I_H6 = 29, I_H7 = 30,
    I_T1_LSB = 31, I_T1_MSB = 32,
    I_GH2_LSB = 33, I_GH2_MSB = 34, I_GH1 = 35, I_GH3 = 36,
    I_RES_HEAT_VAL = 37, I_RES_HEAT_RANGE = 39, I_RANGE_SW_ERR = 41,
};

static uint16_t u16(uint8_t msb, uint8_t lsb) { return (uint16_t)((msb << 8) | lsb); }

void bme68x_parse_calib(const uint8_t a[BME68X_COEFF_LEN], bme68x_calib_t *c) {
    c->par_t1 = u16(a[I_T1_MSB], a[I_T1_LSB]);
    c->par_t2 = (int16_t)u16(a[I_T2_MSB], a[I_T2_LSB]);
    c->par_t3 = (int8_t)a[I_T3];

    c->par_p1  = u16(a[I_P1_MSB], a[I_P1_LSB]);
    c->par_p2  = (int16_t)u16(a[I_P2_MSB], a[I_P2_LSB]);
    c->par_p3  = (int8_t)a[I_P3];
    c->par_p4  = (int16_t)u16(a[I_P4_MSB], a[I_P4_LSB]);
    c->par_p5  = (int16_t)u16(a[I_P5_MSB], a[I_P5_LSB]);
    c->par_p6  = (int8_t)a[I_P6];
    c->par_p7  = (int8_t)a[I_P7];
    c->par_p8  = (int16_t)u16(a[I_P8_MSB], a[I_P8_LSB]);
    c->par_p9  = (int16_t)u16(a[I_P9_MSB], a[I_P9_LSB]);
    c->par_p10 = a[I_P10];

    // H1 i H2 dziela bajt 0xE2: H1 bierze dolna polowe, H2 gorna
    c->par_h1 = (uint16_t)(((uint16_t)a[I_H1_MSB] << 4) | (a[I_H1_LSB] & 0x0F));
    c->par_h2 = (uint16_t)(((uint16_t)a[I_H2_MSB] << 4) | (a[I_H2_LSB] >> 4));
    c->par_h3 = (int8_t)a[I_H3];
    c->par_h4 = (int8_t)a[I_H4];
    c->par_h5 = (int8_t)a[I_H5];
    c->par_h6 = a[I_H6];
    c->par_h7 = (int8_t)a[I_H7];

    c->par_gh1 = (int8_t)a[I_GH1];
    c->par_gh2 = (int16_t)u16(a[I_GH2_MSB], a[I_GH2_LSB]);
    c->par_gh3 = (int8_t)a[I_GH3];

    c->res_heat_range = (uint8_t)((a[I_RES_HEAT_RANGE] & 0x30) >> 4);
    c->res_heat_val   = (int8_t)a[I_RES_HEAT_VAL];
    c->range_sw_err   = (int8_t)(((int8_t)(a[I_RANGE_SW_ERR] & 0xF0)) / 16);
    c->t_fine = 0;
}

float bme68x_temperature(bme68x_calib_t *c, uint32_t adc) {
    float var1 = (((float)adc / 16384.0f) - ((float)c->par_t1 / 1024.0f)) * (float)c->par_t2;
    float var2 = (((float)adc / 131072.0f) - ((float)c->par_t1 / 8192.0f));
    var2 = var2 * var2 * ((float)c->par_t3 * 16.0f);
    c->t_fine = var1 + var2;
    return c->t_fine / 5120.0f;
}

float bme68x_pressure(const bme68x_calib_t *c, uint32_t adc) {
    float var1 = (c->t_fine / 2.0f) - 64000.0f;
    float var2 = var1 * var1 * ((float)c->par_p6 / 131072.0f);
    var2 = var2 + (var1 * (float)c->par_p5 * 2.0f);
    var2 = (var2 / 4.0f) + ((float)c->par_p4 * 65536.0f);
    var1 = ((((float)c->par_p3 * var1 * var1) / 16384.0f) + ((float)c->par_p2 * var1)) / 524288.0f;
    var1 = ((1.0f + (var1 / 32768.0f)) * (float)c->par_p1);
    float p = 1048576.0f - (float)adc;
    if ((int)var1 == 0) return 0;
    p = (((p - (var2 / 4096.0f)) * 6250.0f) / var1);
    var1 = ((float)c->par_p9 * p * p) / 2147483648.0f;
    var2 = p * ((float)c->par_p8 / 32768.0f);
    float var3 = ((p / 256.0f) * (p / 256.0f) * (p / 256.0f) * (c->par_p10 / 131072.0f));
    return p + (var1 + var2 + var3 + ((float)c->par_p7 * 128.0f)) / 16.0f;
}

float bme68x_humidity(const bme68x_calib_t *c, uint16_t adc) {
    float temp_comp = c->t_fine / 5120.0f;
    float var1 = (float)adc - (((float)c->par_h1 * 16.0f) + (((float)c->par_h3 / 2.0f) * temp_comp));
    float var2 = var1 * ((float)(((float)c->par_h2 / 262144.0f) *
                 (1.0f + (((float)c->par_h4 / 16384.0f) * temp_comp) +
                  (((float)c->par_h5 / 1048576.0f) * temp_comp * temp_comp))));
    float var3 = (float)c->par_h6 / 16384.0f;
    float var4 = (float)c->par_h7 / 2097152.0f;
    float h = var2 + ((var3 + (var4 * temp_comp)) * var2 * var2);
    if (h > 100.0f) h = 100.0f;
    else if (h < 0.0f) h = 0.0f;
    return h;
}

float bme68x_gas_high(uint16_t adc, uint8_t range) {
    uint32_t var1 = UINT32_C(262144) >> range;
    int32_t var2 = (int32_t)adc - INT32_C(512);
    var2 *= INT32_C(3);
    var2 = INT32_C(4096) + var2;
    return 1000000.0f * (float)var1 / (float)var2;
}

float bme68x_gas_low(const bme68x_calib_t *c, uint16_t adc, uint8_t range) {
    static const float lookup_k1[16] = {
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, -0.8f,
        0.0f, 0.0f, -0.2f, -0.5f, 0.0f, -1.0f, 0.0f, 0.0f };
    static const float lookup_k2[16] = {
        0.0f, 0.0f, 0.0f, 0.0f, 0.1f, 0.7f, 0.0f, -0.8f,
        -0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    float gas_range_f = (1U << range);
    float var1 = (1340.0f + (5.0f * c->range_sw_err));
    float var2 = (var1) * (1.0f + lookup_k1[range] / 100.0f);
    float var3 = 1.0f + (lookup_k2[range] / 100.0f);
    return 1.0f / (float)(var3 * (0.000000125f) * gas_range_f *
                          ((((float)adc - 512.0f) / var2) + 1.0f));
}

uint8_t bme68x_heater_res(const bme68x_calib_t *c, uint16_t temp, int8_t amb) {
    if (temp > 400) temp = 400;
    float var1 = (((float)c->par_gh1 / (16.0f)) + 49.0f);
    float var2 = ((((float)c->par_gh2 / (32768.0f)) * (0.0005f)) + 0.00235f);
    float var3 = ((float)c->par_gh3 / (1024.0f));
    float var4 = (var1 * (1.0f + (var2 * (float)temp)));
    float var5 = (var4 + (var3 * (float)amb));
    float r = 3.4f * ((var5 * (4 / (4 + (float)c->res_heat_range)) *
                       (1 / (1 + ((float)c->res_heat_val * 0.002f)))) - 25);
    // Przy uszkodzonej kalibracji wynik moze wyjsc poza bajt - rzutowanie
    // takiej liczby na uint8_t to w C zachowanie niezdefiniowane.
    if (!(r >= 0)) return 0;
    if (r > 255) return 255;
    return (uint8_t)r;
}

uint8_t bme68x_gas_wait(uint16_t dur) {
    if (dur >= 0xFC0) return 0xFF;
    uint8_t factor = 0;
    while (dur > 0x3F) {
        dur = dur / 4;
        factor += 1;
    }
    return (uint8_t)(dur + (factor * 64));
}
