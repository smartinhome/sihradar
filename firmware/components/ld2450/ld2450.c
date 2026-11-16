#include "ld2450.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "LD2450";

#define BAUD            256000
#define RX_BUF          1024
#define OFFLINE_US      1000000     // brak ramki przez 1 s = radar niedostepny
#define CONFIG_KICK_US  5000000     // po tylu s ciszy wysylamy "koniec konfiguracji"

static int              s_uart = -1;
static ld2450_data_t    s_data;
static int64_t          s_last_us = 0;
static portMUX_TYPE     s_mux = portMUX_INITIALIZER_UNLOCKED;
static ld2450_parser_t  s_parser;

static void send_cmd(uint16_t cmd, const uint8_t *val, uint16_t vlen) {
    uint8_t b[24];
    int n = ld2450_build_cmd(b, cmd, val, vlen);
    uart_write_bytes(s_uart, b, n);
}

// Odczyt wersji wymaga trybu konfiguracji, w ktorym radar NIE wysyla danych.
// Dlatego zawsze konczymy go komenda CONFIG_OFF (a gdyby zginela - patrz
// CONFIG_KICK_US w zadaniu odbioru).
static void request_fw(void) {
    static const uint8_t on[2] = { 0x01, 0x00 };
    send_cmd(LD2450_CMD_CONFIG_ON, on, sizeof(on));
    vTaskDelay(pdMS_TO_TICKS(120));
    send_cmd(LD2450_CMD_READ_FW, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    send_cmd(LD2450_CMD_CONFIG_OFF, NULL, 0);
}

static void handle_data(const uint8_t *frame) {
    ld2450_target_t t[LD2450_MAX_TARGETS];
    ld2450_decode_targets(frame, t);
    uint8_t cnt = 0;
    for (int i = 0; i < LD2450_MAX_TARGETS; i++) if (t[i].valid) cnt++;
    portENTER_CRITICAL(&s_mux);
    memcpy(s_data.t, t, sizeof(t));
    s_data.count = cnt;
    s_data.frames++;
    s_last_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_mux);
}

static void rx_task(void *arg) {
    (void)arg;
    uint8_t rx[128];
    int fw_tries = 0;
    int64_t next_fw_us = esp_timer_get_time() + 1500000;
    int64_t last_kick_us = 0;
    int64_t sec_start = esp_timer_get_time();
    uint32_t sec_frames = 0;

    while (1) {
        int n = uart_read_bytes(s_uart, rx, sizeof(rx), pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) {
            uint16_t before = s_parser.len;
            ld2450_frame_t f = ld2450_parser_feed(&s_parser, rx[i]);
            if (f == LD2450_FRAME_DATA) {
                handle_data(s_parser.buf);
                sec_frames++;
            } else if (f == LD2450_FRAME_ACK) {
                char fw[24];
                if (ld2450_parse_fw(s_parser.buf, s_parser.flen, fw, sizeof(fw))) {
                    portENTER_CRITICAL(&s_mux);
                    strlcpy(s_data.fw, fw, sizeof(s_data.fw));
                    portEXIT_CRITICAL(&s_mux);
                    ESP_LOGI(TAG, "Firmware radaru: %s", fw);
                }
            } else if (before > 0 && s_parser.len == 0) {
                s_data.errors++;      // ramka urwana lub z blednym ogonem
            }
        }

        int64_t now = esp_timer_get_time();
        if (now - sec_start >= 1000000) {
            portENTER_CRITICAL(&s_mux);
            s_data.fps = (uint8_t)(sec_frames > 255 ? 255 : sec_frames);
            portEXIT_CRITICAL(&s_mux);
            sec_frames = 0;
            sec_start = now;
        }
        if (!s_data.fw[0] && fw_tries < 3 && now >= next_fw_us) {
            fw_tries++;
            next_fw_us = now + 3000000;
            request_fw();
            last_kick_us = esp_timer_get_time();
        }
        // Radar milczy: moze utknal w trybie konfiguracji (zgubiona komenda konca)
        if (now - s_last_us > CONFIG_KICK_US && now - last_kick_us > CONFIG_KICK_US) {
            last_kick_us = now;
            send_cmd(LD2450_CMD_CONFIG_OFF, NULL, 0);
        }
    }
}

void ld2450_init(int uart_num, int pin_tx, int pin_rx) {
    s_uart = uart_num;
    ld2450_parser_reset(&s_parser);
    memset(&s_data, 0, sizeof(s_data));
    uart_config_t cfg = {
        .baud_rate = BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t e = uart_driver_install(uart_num, RX_BUF, 0, 0, NULL, 0);
    if (e == ESP_OK) e = uart_param_config(uart_num, &cfg);
    if (e == ESP_OK) e = uart_set_pin(uart_num, pin_tx, pin_rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "UART%d: blad konfiguracji (%d)", uart_num, e);
        return;
    }
    xTaskCreate(rx_task, "ld2450", 3072, NULL, 6, NULL);
    ESP_LOGI(TAG, "UART%d %d bd, TX=GPIO%d RX=GPIO%d", uart_num, BAUD, pin_tx, pin_rx);
}

void ld2450_get(ld2450_data_t *out) {
    portENTER_CRITICAL(&s_mux);
    *out = s_data;
    int64_t last = s_last_us;
    portEXIT_CRITICAL(&s_mux);
    out->online = last && (esp_timer_get_time() - last) < OFFLINE_US;
    if (!out->online) {
        out->count = 0;
        for (int i = 0; i < LD2450_MAX_TARGETS; i++) out->t[i].valid = false;
    }
}
