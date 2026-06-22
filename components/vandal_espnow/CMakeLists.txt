idf_component_register(
    SRCS "vandal_espnow.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event vandal_common
)
