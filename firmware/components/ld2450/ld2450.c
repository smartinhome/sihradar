#include "ld2450.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "LD2450";

#define RX_BUF          1024
#define OFFLINE_US      1000000     // brak ramki przez 1 s = radar niedostepny
#define SILENT_US       2500000     // tyle bez poprawnej ramki = sprobuj innej predkosci
#define RESTORE_FRAMES  5           // ramek na obcej predkosci, zanim przestawimy radar
#define MAX_RESTORE     2           // prob przywrocenia 256000 na jedno uruchomienie

// Firmware radaru 2.14 po aktualizacji przez HLKRadarTool potrafi zmienic
// predkosc UART (zglaszaja to uzytkownicy Everything Presence i ESPHome).
// Gdy radar milczy, przechodzimy kolejno po predkosciach, ktore obsluguje.
// Po znalezieniu ustawiamy mu z powrotem 256000 - wtedy aplikacja HLK i inne
// sterowniki tez dzialaja; gdyby sie nie dalo, pracujemy na znalezionej.
static const uint32_t BAUDS[] = { 256000, 115200, 460800, 230400, 57600, 38400, 19200, 9600 };
#define BAUD_N (sizeof(BAUDS) / sizeof(BAUDS[0]))

static int              s_uart = -1;
static ld2450_data_t    s_data;
static int64_t          s_last_us = 0;
static portMUX_TYPE     s_mux = portMUX_INITIALIZER_UNLOCKED;
static ld2450_parser_t  s_parser;
static int              s_bi = 0;              // indeks w BAUDS
static int64_t          s_baud_since_us = 0;
static int              s_frames_at_baud = 0;

static void send_cmd(uint16_t cmd, const uint8_t *val, uint16_t vlen) {
    uint8_t b[24];
    int n = ld2450_build_cmd(b, cmd, val, vlen);
    uart_write_bytes(s_uart, b, n);
}

static void config_on(void) {
    static const uint8_t on[2] = { 0x01, 0x00 };
    send_cmd(LD2450_CMD_CONFIG_ON, on, sizeof(on));
    vTaskDelay(pdMS_TO_TICKS(150));
}

// Odczyt wersji wymaga trybu konfiguracji, w ktorym radar NIE wysyla danych.
// Dlatego zawsze konczymy go komenda CONFIG_OFF (a gdyby zginela - wysylamy
// ja ponownie przy kazdej zmianie predkosci, patrz set_baud).
static void request_fw(void) {
    config_on();
    send_cmd(LD2450_CMD_READ_FW, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    send_cmd(LD2450_CMD_CONFIG_OFF, NULL, 0);
}

static void set_baud(int idx) {
    s_bi = idx;
    uart_set_baudrate(s_uart, BAUDS[idx]);
    uart_flush_input(s_uart);
    ld2450_parser_reset(&s_parser);
    s_baud_since_us = esp_timer_get_time();
    s_frames_at_baud = 0;
    portENTER_CRITICAL(&s_mux);
    s_data.baud = BAUDS[idx];
    portEXIT_CRITICAL(&s_mux);
    // Radar mogl utknac w trybie konfiguracji (zgubiona komenda konca)
    send_cmd(LD2450_CMD_CONFIG_OFF, NULL, 0);
}

static void restore_default_baud(void) {
    uint16_t idx = ld2450_baud_index(LD2450_DEFAULT_BAUD);
    uint8_t v[2] = { (uint8_t)(idx & 0xFF), (uint8_t)(idx >> 8) };
    ESP_LOGW(TAG, "Radar nadaje z predkoscia %lu bd - przestawiam go na %d bd",
             (unsigned long)BAUDS[s_bi], LD2450_DEFAULT_BAUD);
    config_on();
    send_cmd(LD2450_CMD_SET_BAUD, v, sizeof(v));
    vTaskDelay(pdMS_TO_TICKS(150));
    send_cmd(LD2450_CMD_RESTART, NULL, 0);    // nowa predkosc dziala po restarcie
    uart_wait_tx_done(s_uart, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(1500));
    set_baud(0);
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
    uint8_t raw[sizeof(s_data.raw)];
    int raw_len = 0;
    int fw_tries = 0, restores = 0;
    int64_t next_fw_us = 0;
    int64_t sec_start = esp_timer_get_time();
    uint32_t sec_frames = 0;
    bool was_online = false;

    set_baud(0);
    while (1) {
        int n = uart_read_bytes(s_uart, rx, sizeof(rx), pdMS_TO_TICKS(20));
        uint32_t errs = 0;
        for (int i = 0; i < n; i++) {
            uint16_t before = s_parser.len;
            ld2450_frame_t f = ld2450_parser_feed(&s_parser, rx[i]);
            if (f == LD2450_FRAME_DATA) {
                handle_data(s_parser.buf);
                sec_frames++;
                s_frames_at_baud++;
            } else if (f == LD2450_FRAME_ACK) {
                char fw[24];
                if (ld2450_parse_fw(s_parser.buf, s_parser.flen, fw, sizeof(fw))) {
                    portENTER_CRITICAL(&s_mux);
                    strlcpy(s_data.fw, fw, sizeof(s_data.fw));
                    portEXIT_CRITICAL(&s_mux);
                    ESP_LOGI(TAG, "Firmware radaru: %s", fw);
                }
            } else if (before > 0 && s_parser.len == 0) {
                errs++;      // ramka urwana lub z blednym ogonem
            }
        }
        // Ostatnie bajty do podgladu w panelu (pomaga odroznic zla predkosc
        // - przypadkowe bajty - od nieznanego formatu ramek)
        if (n > 0) {
            int keep = (int)sizeof(raw);
            if (n >= keep) {
                memcpy(raw, rx + n - keep, keep);
                raw_len = keep;
            } else {
                int drop = raw_len + n - keep;
                if (drop > 0) { memmove(raw, raw + drop, raw_len - drop); raw_len -= drop; }
                memcpy(raw + raw_len, rx, n);
                raw_len += n;
            }
            portENTER_CRITICAL(&s_mux);
            s_data.rx_bytes += n;
            s_data.errors += errs;
            memcpy(s_data.raw, raw, raw_len);
            s_data.raw_len = (uint8_t)raw_len;
            portEXIT_CRITICAL(&s_mux);
        }

        int64_t now = esp_timer_get_time();
        if (now - sec_start >= 1000000) {
            portENTER_CRITICAL(&s_mux);
            s_data.fps = (uint8_t)(sec_frames > 255 ? 255 : sec_frames);
            portEXIT_CRITICAL(&s_mux);
            sec_frames = 0;
            sec_start = now;
        }

        int64_t quiet_from = s_last_us > s_baud_since_us ? s_last_us : s_baud_since_us;
        bool silent = now - quiet_from > SILENT_US;
        portENTER_CRITICAL(&s_mux);
        s_data.scanning = silent || now - s_last_us > SILENT_US;
        portEXIT_CRITICAL(&s_mux);

        if (silent) {
            if (was_online) {
                // Radar zniknal (restart, aktualizacja przez Bluetooth) - po
                // powrocie wersje odczytamy od nowa
                ESP_LOGW(TAG, "Brak danych z radaru - szukam jego predkosci UART");
                was_online = false;
                portENTER_CRITICAL(&s_mux);
                s_data.fw[0] = 0;
                portEXIT_CRITICAL(&s_mux);
                fw_tries = 0;
            }
            int next = (s_bi + 1) % BAUD_N;
            if (next == 0) ESP_LOGW(TAG, "Radar nie odpowiada na zadnej predkosci - sprawdz zasilanie 5 V i przewody");
            set_baud(next);
            continue;
        }
        if (s_frames_at_baud > 0 && !was_online) {
            was_online = true;
            next_fw_us = now + 500000;
            ESP_LOGI(TAG, "Radar odpowiada na %lu bd", (unsigned long)BAUDS[s_bi]);
        }
        if (BAUDS[s_bi] != LD2450_DEFAULT_BAUD && s_frames_at_baud >= RESTORE_FRAMES &&
            restores < MAX_RESTORE) {
            restores++;
            restore_default_baud();
            was_online = false;
            continue;
        }
        if (was_online && !s_data.fw[0] && fw_tries < 3 && now >= next_fw_us) {
            fw_tries++;
            next_fw_us = now + 3000000;
            request_fw();
        }
    }
}

void ld2450_init(int uart_num, int pin_tx, int pin_rx) {
    s_uart = uart_num;
    ld2450_parser_reset(&s_parser);
    memset(&s_data, 0, sizeof(s_data));
    uart_config_t cfg = {
        .baud_rate = LD2450_DEFAULT_BAUD,
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
    xTaskCreate(rx_task, "ld2450", 3584, NULL, 6, NULL);
    ESP_LOGI(TAG, "UART%d TX=GPIO%d RX=GPIO%d, start %d bd", uart_num, pin_tx, pin_rx,
             LD2450_DEFAULT_BAUD);
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
