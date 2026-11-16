#include "led_status.h"
#include "wifi_manager.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

#define RMT_RESOLUTION  10000000   // 10 MHz -> 1 tick = 100 ns
#define BRIGHTNESS      12         // % - dioda swieci prosto w oczy z bliska

static const char *TAG = "LED";

static rmt_channel_handle_t s_chan = NULL;
static rmt_encoder_handle_t s_encoder = NULL;
static volatile led_hint_t  s_hint = LED_HINT_NONE;

// --- Enkoder WS2812 (jak w SIHOS17) ---
// T0H=0.4us T0L=0.85us, T1H=0.8us T1L=0.45us; przy 100 ns/tick: 4/8 i 8/4
static const rmt_symbol_word_t s_bit0 = { .level0 = 1, .duration0 = 4, .level1 = 0, .duration1 = 8 };
static const rmt_symbol_word_t s_bit1 = { .level0 = 1, .duration0 = 8, .level1 = 0, .duration1 = 4 };

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} ws2812_encoder_t;

static size_t ws2812_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                            const void *primary_data, size_t data_size,
                            rmt_encode_state_t *ret_state) {
    ws2812_encoder_t *enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encode_state_t session = RMT_ENCODING_RESET;
    size_t encoded = 0;
    if (enc->state == 0) {
        encoded += enc->bytes_encoder->encode(enc->bytes_encoder, channel, primary_data, data_size, &session);
        if (session & RMT_ENCODING_COMPLETE) enc->state = 1;
        if (session & RMT_ENCODING_MEM_FULL) { *ret_state = RMT_ENCODING_MEM_FULL; return encoded; }
    }
    if (enc->state == 1) {
        encoded += enc->copy_encoder->encode(enc->copy_encoder, channel, &enc->reset_code,
                                             sizeof(enc->reset_code), &session);
        if (session & RMT_ENCODING_COMPLETE) { enc->state = RMT_ENCODING_RESET; *ret_state = RMT_ENCODING_COMPLETE; }
        if (session & RMT_ENCODING_MEM_FULL) { *ret_state = RMT_ENCODING_MEM_FULL; return encoded; }
    }
    return encoded;
}

static esp_err_t ws2812_del(rmt_encoder_t *encoder) {
    ws2812_encoder_t *enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_del_encoder(enc->bytes_encoder);
    rmt_del_encoder(enc->copy_encoder);
    free(enc);
    return ESP_OK;
}

static esp_err_t ws2812_reset(rmt_encoder_t *encoder) {
    ws2812_encoder_t *enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_reset(enc->bytes_encoder);
    rmt_encoder_reset(enc->copy_encoder);
    enc->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

static esp_err_t make_encoder(rmt_encoder_handle_t *out) {
    ws2812_encoder_t *enc = calloc(1, sizeof(ws2812_encoder_t));
    if (!enc) return ESP_ERR_NO_MEM;
    enc->base.encode = ws2812_encode;
    enc->base.del = ws2812_del;
    enc->base.reset = ws2812_reset;
    rmt_bytes_encoder_config_t bcfg = { .bit0 = s_bit0, .bit1 = s_bit1, .flags = { .msb_first = 1 } };
    if (rmt_new_bytes_encoder(&bcfg, &enc->bytes_encoder) != ESP_OK) { free(enc); return ESP_FAIL; }
    rmt_copy_encoder_config_t ccfg = {};
    if (rmt_new_copy_encoder(&ccfg, &enc->copy_encoder) != ESP_OK) {
        rmt_del_encoder(enc->bytes_encoder);
        free(enc);
        return ESP_FAIL;
    }
    enc->reset_code = (rmt_symbol_word_t){ .level0 = 0, .duration0 = 250, .level1 = 0, .duration1 = 250 };
    *out = &enc->base;
    return ESP_OK;
}

static void led_show(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_chan || !s_encoder) return;
    // Waveshare ESP32-C6-Zero: dioda przyjmuje kolejnosc RGB (nie GRB).
    // Gdyby kolory wyszly zamienione (czerwony <-> zielony) - zamien r i g.
    uint8_t px[3] = {
        (uint8_t)((uint16_t)r * BRIGHTNESS / 100),
        (uint8_t)((uint16_t)g * BRIGHTNESS / 100),
        (uint8_t)((uint16_t)b * BRIGHTNESS / 100),
    };
    rmt_transmit_config_t tx = { .loop_count = 0 };
    rmt_transmit(s_chan, s_encoder, px, sizeof(px), &tx);
    rmt_tx_wait_all_done(s_chan, 50);
}

static void led_task(void *arg) {
    (void)arg;
    uint32_t last = 0xFFFFFFFF;
    int tick = 0;
    while (1) {
        uint32_t c;
        wifi_state_t st = wifi_manager_get_state();
        if (s_hint == LED_HINT_BUTTON) {
            c = 0xFFFFFF;
        } else if (wifi_manager_test_state() == WIFI_TEST_RUNNING) {
            c = (tick & 1) ? 0xFFB000 : 0;                 // zolta, mruga
        } else if (st == WIFI_STATE_CONNECTED) {
            c = 0x00FF00;
        } else if (st == WIFI_STATE_AP_MODE) {
            c = wifi_manager_ap_clients() > 0 ? 0x0000FF : 0xFF0000;
        } else {
            c = 0xFF0000;
        }
        if (c != last) {
            last = c;
            led_show((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
        }
        tick++;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void led_status_init(int gpio) {
    rmt_tx_channel_config_t chan_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = gpio,
        .mem_block_symbols = 48,
        .resolution_hz = RMT_RESOLUTION,
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&chan_cfg, &s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "Nie udalo sie utworzyc kanalu RMT");
        return;
    }
    if (make_encoder(&s_encoder) != ESP_OK) {
        ESP_LOGE(TAG, "Nie udalo sie utworzyc enkodera WS2812");
        return;
    }
    rmt_enable(s_chan);
    led_show(255, 0, 0);
    xTaskCreate(led_task, "led", 2560, NULL, 2, NULL);
}

void led_status_hint(led_hint_t hint) { s_hint = hint; }
