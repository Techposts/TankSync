idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        sensor_sr04
        battery_monitor
        lora_tx
        wifi_ota
        led_ws2812
        nvs_flash
        esp_timer
        freertos
        driver
        app_update
)
