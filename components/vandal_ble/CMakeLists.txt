idf_component_register(
    SRCS "vandal_ble.c"
    INCLUDE_DIRS "include"
    REQUIRES bt vandal_common esp_event
)
