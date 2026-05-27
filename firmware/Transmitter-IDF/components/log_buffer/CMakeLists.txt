idf_component_register(
    SRCS "log_buffer.c"
    INCLUDE_DIRS "include"
    REQUIRES
        log
        freertos
)
