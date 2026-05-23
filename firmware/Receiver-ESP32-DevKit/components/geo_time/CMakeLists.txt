idf_component_register(
    SRCS "geo_time.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_http_client
        esp_netif
        esp_wifi
        lwip
        nvs_flash
        json
        freertos
)
