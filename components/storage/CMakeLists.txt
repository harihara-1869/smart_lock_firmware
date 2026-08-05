idf_component_register(
    SRCS
        "src/key_store_ram.c"
        "src/intent_log_ram.c"
    INCLUDE_DIRS
        "include"
    # RAM placeholder needs no public dependencies beyond ESP-IDF common.
    # PRIV_REQUIRES nvs_flash / secure-element driver is added here ONLY when
    # those backends land — nvs_flash must never appear in app_module's own
    # dependency list.
    PRIV_REQUIRES
        esp_common
        log
)
