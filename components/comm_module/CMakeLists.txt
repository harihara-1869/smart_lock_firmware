idf_component_register(
    SRCS "src/comm_module.c"
    INCLUDE_DIRS "include"
    REQUIRES session transport esp_driver_i2c
)
