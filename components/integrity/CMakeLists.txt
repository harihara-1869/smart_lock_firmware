idf_component_register(
    SRCS
        "src/integrity.c"
        "src/integrity_backend_stub.c"
        "src/integrity_backend_tamper_gpio.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        # Public header exposes only bool.
        esp_common
    PRIV_REQUIRES
        esp_driver_gpio
        log
)
