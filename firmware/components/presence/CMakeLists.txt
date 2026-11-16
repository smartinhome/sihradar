idf_component_register(
    SRCS "presence.c" "presence_core.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash log esp_timer nvs_config ld2450 room
)
