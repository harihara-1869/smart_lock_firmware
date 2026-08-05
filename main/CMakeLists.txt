idf_component_register(SRCS "smart_lock_firmware.c" "test_integration.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES app_module comm_module test_utils actuator display integrity storage)
