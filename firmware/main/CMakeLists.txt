idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_netif esp_event esp_app_format driver log
             nvs_config wifi_manager webserver led_status ld2450 bme688 ota_manager room
)
