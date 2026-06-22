idf_component_register(
    SRCS "vandal_spi.c"
    INCLUDE_DIRS "include"
    REQUIRES driver vandal_common esp_event
)
