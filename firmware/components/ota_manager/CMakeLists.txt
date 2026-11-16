idf_component_register(
    SRCS "ota_manager.c"
    INCLUDE_DIRS "."
    REQUIRES app_update esp_http_client esp-tls mbedtls esp_app_format bootloader_support
             esp_timer log nvs_config wifi_manager
)
