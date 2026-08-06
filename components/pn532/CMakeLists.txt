# PN532 NFC driver component (physical/link layer only).
#
# NOTE: this component's directory is "driver", which is ALSO the name of an
# ESP-IDF built-in component. A project component that shares a built-in name
# overrides the built-in one, which breaks every component that depends on the
# real "driver". If you hit build errors mentioning a missing/duplicate
# "driver" component, rename this directory (e.g. to "pn532") — the sources and
# headers below are otherwise self-contained and location-independent.

idf_component_register(
    SRCS
        "src/pn532.c"
        "src/pn532_i2c.c"
        "src/pn532_cmd.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        # Public headers expose I2C types (pn532_i2c.h) and esp_err_t (pn532.h).
        esp_driver_i2c
    PRIV_REQUIRES
        # Implementation-only dependencies.
        esp_driver_gpio
        esp_common
        freertos
)
