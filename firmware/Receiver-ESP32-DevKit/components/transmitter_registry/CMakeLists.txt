idf_component_register(
    SRCS "transmitter_registry.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_timer freertos json spiffs
)
