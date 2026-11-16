idf_component_register(
    SRCS "wifi_manager.c" "captive_dns.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_config esp_wifi esp_netif esp_event esp_timer esp_hw_support log lwip
)
