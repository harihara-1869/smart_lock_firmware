idf_component_register(
    SRCS
        "src/app_module.c"
        "src/app_dispatch.c"
        "src/provision_mgr.c"
        "src/provision_button.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        # The Application depends ONLY on the comm facade plus the peer
        # interfaces. session/transport/lli are PRIV_REQUIRES of comm_module
        # and never appear here.
        comm_module
        actuator
        display
        integrity
        storage
    PRIV_REQUIRES
        # Implementation-only dependencies. nvs_flash is deliberately absent:
        # persistence is owned by the storage component's backends, never by
        # the Application.
        esp_common
        esp_hw_support
        esp_timer
        esp_driver_gpio
        freertos
        log
)
