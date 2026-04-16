idf_component_register(
    SRCS "display_sh1106.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer freertos
)
