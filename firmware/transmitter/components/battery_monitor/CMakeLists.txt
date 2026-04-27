idf_component_register(
    SRCS "battery_monitor.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_adc
        driver        # i2c_master
        nvs_flash     # power-mode override persistence
        freertos
)
