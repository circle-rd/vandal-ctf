idf_component_register(
    SRCS "vandal_http.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_http_client json esp_event vandal_common
             vandal_uart vandal_i2c vandal_spi vandal_espnow vandal_ble vandal_thread
             driver esp_timer esp_system
)
