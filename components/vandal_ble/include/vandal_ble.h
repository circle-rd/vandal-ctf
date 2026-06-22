/*
 * Vandal Test - BLE module header.
 *
 * Exposes two GATT services via NimBLE:
 *
 *   1. Open service   – readable by anyone, no pairing needed.
 *   2. Auth service    – readable only after numeric-comparison pairing
 *                        with the PIN from KConfig.
 *
 * Master: acts as GATT server, periodically updates characteristic values.
 * Slave:  acts as GATT client, reads characteristics and posts events.
 */

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Initialize and start the BLE module (slave: called at boot).
     */
    void vandal_ble_init(void);

    /**
     * @brief Ensure BLE stack is initialized and advertising (master only, lazy init).
     *        Call this before setting BLE_OPEN/BLE_AUTH running flags.
     */
    void vandal_ble_start(void);

    /**
     * @brief Stop BLE payload updates (master only).
     */
    void vandal_ble_stop(void);

#ifdef __cplusplus
}
#endif
