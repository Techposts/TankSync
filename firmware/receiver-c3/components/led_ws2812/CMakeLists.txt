idf_component_register(
    SRCS "led_ws2812.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer
)
