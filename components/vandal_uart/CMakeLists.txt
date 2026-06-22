idf_component_register(
    SRCS "vandal_uart.c"
    INCLUDE_DIRS "include"
    REQUIRES driver vandal_common esp_event
)
