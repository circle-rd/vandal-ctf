idf_component_register(
    SRCS "vandal_i2c.c"
    INCLUDE_DIRS "include"
    REQUIRES driver vandal_common esp_event
)
