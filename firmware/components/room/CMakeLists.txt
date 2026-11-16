idf_component_register(
    SRCS "room.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash log nvs_config
)
