idf_component_register(
    SRCS "wifi_ota.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_http_server app_update nvs_flash esp_common log esp_timer esp_app_format driver battery_monitor
)
