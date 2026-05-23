idf_component_register(
    SRCS "ota_manager.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_https_ota
        esp_http_client
        app_update
        mbedtls
        json
        freertos
        esp_timer
        buzzer
)
