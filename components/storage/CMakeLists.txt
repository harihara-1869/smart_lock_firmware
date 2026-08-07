idf_component_register(
    SRCS
        "src/key_store.c"
        "src/intent_log.c"
        "src/storage_hal.c"
    INCLUDE_DIRS
        "include"
    # nvs_flash is a PRIV_REQUIRES so nvs.h never leaks into any component
    # that depends on storage. The Application and all peers call only the
    # semantic interfaces in key_store.h / intent_log.h.
    PRIV_REQUIRES
        nvs_flash
        esp_common
        log
)
