idf_component_register(
    SRCS
        "src/aai.c"
        "src/aai_backend_stub.c"
        "src/aai_backend_rmt.c"
        "src/aai_backend_mcpwm.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        # Public header exposes only stdint/stdbool types.
        esp_common
    PRIV_REQUIRES
        # Stub backend uses esp_timer; hardware backends will use the RMT /
        # MCPWM / GPIO drivers. Listed unconditionally so every backend
        # compiles in every test mode; the active backend is selected by
        # CONFIG_ACTUATOR_BACKEND_* at runtime-of-compile.
        esp_timer
        esp_driver_rmt
        esp_driver_mcpwm
        esp_driver_gpio
        log
)
