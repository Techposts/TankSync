idf_component_register(
    SRCS "sensor_iface.c"
    INCLUDE_DIRS "include"
    REQUIRES
        sensor_sr04
        sensor_ld2413
)
