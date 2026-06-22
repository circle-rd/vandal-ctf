idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES vandal_common vandal_election vandal_uart vandal_i2c vandal_spi
             vandal_wifi vandal_espnow vandal_ble vandal_http vandal_thread
             driver esp_event esp_wifi bt nvs_flash
             esp_http_server esp_http_client json
             fatfs wear_levelling vfs
             openthread
)
