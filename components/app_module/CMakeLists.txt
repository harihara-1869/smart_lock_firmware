idf_component_register(SRCS "src/provision_mgr.c" "src/app_display_console.c" "src/nvs_store.c"
                       INCLUDE_DIRS "include"
                       REQUIRES comm_module esp_timer esp_hw_support)
