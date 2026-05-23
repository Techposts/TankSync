idf_component_register(
    SRCS "buzzer.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        nvs_flash
        freertos
        esp_timer
)
