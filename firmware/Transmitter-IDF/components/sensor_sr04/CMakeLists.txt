idf_component_register(
    SRCS "sensor_sr04.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_timer
        freertos
)
