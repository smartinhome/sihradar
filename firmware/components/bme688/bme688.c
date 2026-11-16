#include "bme688.h"
#include "bme68x_calc.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "BME688";

#define REG_FIELD0     0x1D   // 17 B: status, cisnienie, temperatura, wilgotnosc, gaz
#define REG_RES_HEAT0  0x5A
#define REG_GAS_WAIT0  0x64
#define REG_CTRL_GAS0  0x70
#define REG_CTRL_GAS1  0x71
#define REG_CTRL_HUM   0x72
#define REG_CTRL_MEAS  0x74
#define REG_CONFIG     0x75
#define REG_CHIP_ID    0xD0
#define REG_RESET      0xE0
#define REG_VARIANT    0xF0
#define CHIP_ID        0x61

// Nadprobkowanie: T x2, P x4, H x2; filtr IIR 3; grzalka 320 °C przez 150 ms
#define OS_T           2
#define OS_P           3
#define OS_H           2
#define FILTER         2
#define HEAT_C         320
#define HEAT_MS        150
#define PERIOD_MS      3000
#define STALE_US       10000000   // pomiar starszy niz 10 s = czujnik niedostepny

static int                     s_sda, s_scl;
static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static bme68x_calib_t          s_cal;
static bool                    s_high = true;   // BME688 = wariant "gas high"
static bme688_data_t           s_data;
static int64_t                 s_last_us = 0;
static portMUX_TYPE            s_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t len) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 100);
}

static esp_err_t wr(uint8_t reg, uint8_t val) {
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, 100);
}

static void drop_device(void) {
    if (s_dev) i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    portENTER_CRITICAL(&s_mux);
    s_data.addr = 0;
    s_data.chip[0] = 0;
    portEXIT_CRITICAL(&s_mux);
}

static bool find_sensor(void) {
    static const uint8_t addrs[2] = { 0x77, 0x76 };
    for (int i = 0; i < 2; i++) {
        if (i2c_master_probe(s_bus, addrs[i], 50) != ESP_OK) continue;
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(s_bus, &dc, &s_dev) != ESP_OK) continue;
        uint8_t id = 0;
        if (rd(REG_CHIP_ID, &id, 1) != ESP_OK || id != CHIP_ID) {
            ESP_LOGW(TAG, "Pod adresem 0x%02X jest inny uklad (ID 0x%02X)", addrs[i], id);
            drop_device();
            continue;
        }
        wr(REG_RESET, 0xB6);
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t variant = 0;
        uint8_t coeff[BME68X_COEFF_LEN];
        if (rd(REG_VARIANT, &variant, 1) != ESP_OK ||
            rd(BME68X_COEFF1_ADDR, coeff, BME68X_COEFF1_LEN) != ESP_OK ||
            rd(BME68X_COEFF2_ADDR, coeff + BME68X_COEFF1_LEN, BME68X_COEFF2_LEN) != ESP_OK ||
            rd(BME68X_COEFF3_ADDR, coeff + BME68X_COEFF1_LEN + BME68X_COEFF2_LEN, BME68X_COEFF3_LEN) != ESP_OK) {
            ESP_LOGW(TAG, "Nie udalo sie odczytac kalibracji");
            drop_device();
            continue;
        }
        bme68x_parse_calib(coeff, &s_cal);
        s_high = (variant == 0x01);
        portENTER_CRITICAL(&s_mux);
        s_data.addr = addrs[i];
        strlcpy(s_data.chip, s_high ? "BME688" : "BME680", sizeof(s_data.chip));
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "Znaleziono %s pod adresem 0x%02X", s_high ? "BME688" : "BME680", addrs[i]);
        return true;
    }
    return false;
}

// Jeden pomiar w trybie wymuszonym (forced): czujnik mierzy raz i zasypia.
static bool measure(void) {
    int8_t amb = 25;
    if (s_data.samples) amb = (int8_t)s_data.temp_c;
    if (wr(REG_CTRL_HUM, OS_H) != ESP_OK) return false;
    wr(REG_CONFIG, FILTER << 2);
    wr(REG_RES_HEAT0, bme68x_heater_res(&s_cal, HEAT_C, amb));
    wr(REG_GAS_WAIT0, bme68x_gas_wait(HEAT_MS));
    wr(REG_CTRL_GAS0, 0x00);                               // grzalka wlaczona
    wr(REG_CTRL_GAS1, (s_high ? 0x20 : 0x10) | 0x00);      // run_gas, profil 0
    if (wr(REG_CTRL_MEAS, (OS_T << 5) | (OS_P << 2) | 0x01) != ESP_OK) return false;

    // Czas pomiaru T/P/H wg Bosch (ok. 21 ms) + grzalka
    vTaskDelay(pdMS_TO_TICKS(25 + HEAT_MS));
    uint8_t f[17];
    for (int i = 0; i < 20; i++) {
        if (rd(REG_FIELD0, f, sizeof(f)) != ESP_OK) return false;
        if (f[0] & 0x80) break;                            // new_data
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!(f[0] & 0x80)) {
        ESP_LOGW(TAG, "Pomiar nie zakonczyl sie w czasie");
        return true;   // czujnik odpowiada, sprobujemy w nastepnym cyklu
    }

    uint32_t adc_p = ((uint32_t)f[2] << 12) | ((uint32_t)f[3] << 4) | (f[4] >> 4);
    uint32_t adc_t = ((uint32_t)f[5] << 12) | ((uint32_t)f[6] << 4) | (f[7] >> 4);
    uint16_t adc_h = (uint16_t)((f[8] << 8) | f[9]);
    float t = bme68x_temperature(&s_cal, adc_t);
    float p = bme68x_pressure(&s_cal, adc_p);
    float h = bme68x_humidity(&s_cal, adc_h);

    float gas = 0;
    bool gas_ok;
    if (s_high) {
        uint16_t adc_g = (uint16_t)((f[15] << 2) | (f[16] >> 6));
        gas_ok = (f[16] & 0x20) && (f[16] & 0x10);          // gas_valid, heat_stab
        if (gas_ok) gas = bme68x_gas_high(adc_g, f[16] & 0x0F);
    } else {
        uint16_t adc_g = (uint16_t)((f[13] << 2) | (f[14] >> 6));
        gas_ok = (f[14] & 0x20) && (f[14] & 0x10);
        if (gas_ok) gas = bme68x_gas_low(&s_cal, adc_g, f[14] & 0x0F);
    }

    portENTER_CRITICAL(&s_mux);
    s_data.temp_c = t;
    s_data.hum_pct = h;
    s_data.press_hpa = p / 100.0f;
    s_data.gas_valid = gas_ok;
    if (gas_ok) s_data.gas_kohm = gas / 1000.0f;
    s_data.samples++;
    s_last_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_mux);
    return true;
}

static void bme_task(void *arg) {
    (void)arg;
    while (1) {
        if (!s_dev && !find_sensor()) {
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        if (!measure()) {
            ESP_LOGW(TAG, "Czujnik przestal odpowiadac");
            drop_device();
        }
        vTaskDelay(pdMS_TO_TICKS(PERIOD_MS));
    }
}

void bme688_init(int pin_sda, int pin_scl) {
    s_sda = pin_sda;
    s_scl = pin_scl;
    memset(&s_data, 0, sizeof(s_data));
    i2c_master_bus_config_t bc = {
        .i2c_port = 0,
        .sda_io_num = s_sda,
        .scl_io_num = s_scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "Blad magistrali I2C (SDA=GPIO%d SCL=GPIO%d)", s_sda, s_scl);
        return;
    }
    xTaskCreate(bme_task, "bme688", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "I2C SDA=GPIO%d SCL=GPIO%d", s_sda, s_scl);
}

void bme688_get(bme688_data_t *out) {
    portENTER_CRITICAL(&s_mux);
    *out = s_data;
    int64_t last = s_last_us;
    portEXIT_CRITICAL(&s_mux);
    out->online = s_dev && last && (esp_timer_get_time() - last) < STALE_US;
}
