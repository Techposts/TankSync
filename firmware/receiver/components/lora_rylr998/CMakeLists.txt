idf_component_register(
    SRCS "lora_rylr998.c"
    INCLUDE_DIRS "."
    REQUIRES driver nvs_flash freertos esp_timer transmitter_registry led_ws2812
)
