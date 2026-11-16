#include "app_main.h"
#include "nvs_config.h"
#include "wifi_manager.h"
#include "webserver.h"
#include "led_status.h"
#include "ld2450.h"
#include "bme688.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <time.h>

static const char *TAG = "MAIN";

// Przycisk BOOT trzymany WIFI_RESET_HOLD_S sekund: zapomnij siec i wroc do AP.
// Jedyna droga ratunku, gdy modul nie widzi sieci, a ktos zmienil haslo routera
// (AP i tak wstaje sam po nieudanym starcie, ale ta droga jest natychmiastowa).
static void button_task(void *arg) {
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    int held_ms = 0;
    while (1) {
        if (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
            held_ms += 50;
            if (held_ms == 1000) led_status_hint(LED_HINT_BUTTON);
            if (held_ms >= WIFI_RESET_HOLD_S * 1000) {
                ESP_LOGW(TAG, "BOOT trzymany %d s - kasuje siec Wi-Fi i restartuje", WIFI_RESET_HOLD_S);
                nvs_config_forget_wifi();
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
            }
        } else {
            if (held_ms >= 1000) led_status_hint(LED_HINT_NONE);
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void) {
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);   // Polska, z automatycznym DST
    tzset();
    ESP_LOGI(TAG, "SIH Radar v%s - start", esp_app_get_description()->version);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    nvs_config_init();
    led_status_init(LED_PIN);

    // Czujniki przed Wi-Fi: wifi_manager_init czeka do 15 s na siec domowa,
    // a pomiary maja byc gotowe, zanim ktos otworzy panel.
    ld2450_init(LD2450_UART_NUM, LD2450_PIN_TX, LD2450_PIN_RX);
    bme688_init(BME688_PIN_SDA, BME688_PIN_SCL);

    wifi_manager_init();
    webserver_init();

    xTaskCreate(button_task, "button", 2560, NULL, 3, NULL);

    ESP_LOGI(TAG, "System gotowy, wolna pamiec %u B", (unsigned)esp_get_free_heap_size());
}
