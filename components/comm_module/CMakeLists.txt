idf_component_register(
    SRCS "src/comm_module.c"
    INCLUDE_DIRS "include"
    REQUIRES
        session
        transport
        lli
)