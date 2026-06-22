idf_component_register(
    SRCS "vandal_thread.c"
    INCLUDE_DIRS "include"
    REQUIRES openthread esp_event esp_netif vandal_common freertos
)
