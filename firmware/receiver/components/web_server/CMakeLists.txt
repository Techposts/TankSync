idf_component_register(
    SRCS "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_http_server
        esp_https_ota
        app_update
        esp_wifi
        json
        nvs_flash
        freertos
        esp_timer
        transmitter_registry
        wifi_manager
        mqtt_client
        ota_manager
        lora_rylr998
        led_ws2812
)
