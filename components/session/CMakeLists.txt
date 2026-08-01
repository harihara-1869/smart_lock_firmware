idf_component_register(
    SRCS
        "src/session.c"
        "src/session_crypto.c"
        "src/third_party/monocypher.c"
        "src/third_party/monocypher-ed25519.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        # Public header exposes transport types (transport_capdu_t etc.).
        transport
    PRIV_REQUIRES
        # Implementation-only dependencies.
        mbedtls
        esp_hw_support
        esp_common
        esp_timer
)
