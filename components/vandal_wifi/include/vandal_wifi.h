/*
 * Vandal Test - WiFi module header.
 *
 * Two-phase initialization for election support:
 *   1. vandal_wifi_init_pre_election()  — STA mode on fixed channel (for ESP-NOW election)
 *   2. vandal_wifi_init_post_election() — Master: switch to AP mode. Slave: connect to master AP.
 *
 * Must be called BEFORE vandal_espnow_init() / vandal_election_run().
 */

#pragma once

#include "vandal_common.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Phase 1: Initialize WiFi in STA mode for election.
     *
     * Sets up NVS, netif, event loop, and starts WiFi in STA mode on
     * the configured channel. ESP-NOW can operate after this call.
     */
    void vandal_wifi_init_pre_election(void);

    /**
     * @brief Phase 2: Reconfigure WiFi based on elected role.
     *
     * Master: stops STA, switches to AP mode (open).
     * Slave:  connects STA to the master's AP SSID.
     *
     * @param role The elected role for this board.
     */
    void vandal_wifi_init_post_election(vandal_role_t role);

#ifdef __cplusplus
}
#endif
