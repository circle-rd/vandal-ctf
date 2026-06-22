/*
 * Vandal Test - ESP-NOW module header.
 *
 * Master: broadcasts payload via ESP-NOW.
 * Slave:  receives ESP-NOW data and posts event.
 *
 * WiFi must already be initialized (vandal_wifi_init) before calling
 * vandal_espnow_init, because ESP-NOW runs on top of the WiFi radio.
 */

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Initialize and start the ESP-NOW module (slave: called at boot).
     */
    void vandal_espnow_init(void);

    /**
     * @brief Start ESP-NOW broadcast (master only, lazy init).
     */
    void vandal_espnow_start(void);

    /**
     * @brief Stop ESP-NOW broadcast (master only).
     */
    void vandal_espnow_stop(void);

#ifdef __cplusplus
}
#endif
