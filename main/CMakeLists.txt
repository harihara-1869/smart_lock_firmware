idf_component_register(SRCS "smart_lock_firmware.c" "test_integration.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES comm_module app_module test_utils)

