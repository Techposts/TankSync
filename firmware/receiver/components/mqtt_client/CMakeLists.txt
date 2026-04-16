idf_component_register(
    SRCS "mqtt_manager.c"
    INCLUDE_DIRS "."
    REQUIRES
        mqtt
        nvs_flash
        esp_wifi
        esp_netif
        esp_timer
        freertos
        transmitter_registry
        wifi_manager
)
