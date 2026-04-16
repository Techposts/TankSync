idf_component_register(
    SRCS "lora_tx.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        nvs_flash
        freertos
        esp_timer
)
