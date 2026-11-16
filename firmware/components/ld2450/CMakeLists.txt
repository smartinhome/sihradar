idf_component_register(
    SRCS "ld2450.c" "ld2450_proto.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer log
)
