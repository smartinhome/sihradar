idf_component_register(
    SRCS "bme688.c" "bme68x_calc.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer log
)
