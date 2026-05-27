idf_component_register(
    SRCS "sensor_ld2413.c"
    INCLUDE_DIRS "include"
    REQUIRES
        driver
        esp_timer
        freertos
)
