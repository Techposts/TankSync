idf_component_register(
    SRCS "battery_monitor.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_adc
        driver
        freertos
)
