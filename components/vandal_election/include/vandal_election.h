/*
 * Vandal Test - Boot-time master election via ESP-NOW broadcast.
 *
 * Algorithm:
 *   1. Both boards init WiFi in STA mode (same channel) + ESP-NOW.
 *   2. Each generates a random election_id (uint32_t).
 *   3. Boards broadcast ELECTION_CLAIM messages every 200ms.
 *   4. If a higher election_id is received → yield (become slave).
 *   5. Timeout with no higher claim → declare self master, broadcast RESULT.
 *   6. Slave waits for ELECTION_RESULT before returning.
 *
 * Tie-break: if election_ids are equal, higher MAC address wins.
 */

#pragma once

#include "vandal_common.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Run the master election protocol (blocking).
     *
     * WiFi must already be started in STA mode on the correct channel
     * (call vandal_wifi_init_pre_election() first).
     *
     * Returns the elected role for this board. Also sets the role
     * internally via vandal_set_role().
     */
    vandal_role_t vandal_election_run(void);

#ifdef __cplusplus
}
#endif
