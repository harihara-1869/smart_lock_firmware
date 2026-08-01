idf_component_register(
    SRCS
        "src/lli.c"
        "src/lli_mock.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        # Public header exposes i2c_port_t (lli.h).
        esp_driver_i2c
    PRIV_REQUIRES
        pn532
)
