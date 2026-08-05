idf_component_register(
    SRCS
        "src/display.c"
        "src/display_backend_console.c"
        "src/display_backend_panel.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        # Public header exposes only stdint/stdbool/stddef types.
        esp_common
    PRIV_REQUIRES
        # Console backend uses esp_log; panel backend will use SPI/GPIO.
        esp_driver_gpio
        esp_driver_spi
        log
)
