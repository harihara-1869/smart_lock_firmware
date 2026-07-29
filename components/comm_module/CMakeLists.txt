idf_component_register(
    SRCS "src/comm_module.c"
    INCLUDE_DIRS "include"
    REQUIRES
        # Public header (comm_module.h) exposes i2c_port_t and FreeRTOS
        # task types — these are public dependencies.
        esp_driver_i2c
        freertos
    PRIV_REQUIRES
        # Implementation-only: comm_module.h leaks none of their types, so
        # their include dirs must NOT propagate to components that depend on
        # comm_module (keeps the "Application depends only on comm_module.h"
        # boundary real at the build-system level).
        session
        transport
        lli
)