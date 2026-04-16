idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        lora_rylr998
        transmitter_registry
        wifi_manager
        web_server
        mqtt_client
        ota_manager
        display_sh1106
        led_ws2812
        nvs_flash
        esp_timer
        freertos
)
