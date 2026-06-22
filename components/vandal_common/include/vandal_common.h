/*
 * Vandal Test - Common types, event bus, and payload generator.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_event.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* ---------------------------------------------------------------------------
     * Board role (determined at runtime via election)
     * --------------------------------------------------------------------------- */
    typedef enum
    {
        VANDAL_ROLE_MASTER = 0,
        VANDAL_ROLE_SLAVE = 1,
    } vandal_role_t;

    /**
     * @brief Get the current board role (valid after election completes).
     */
    vandal_role_t vandal_get_role(void);

    /**
     * @brief Set the board role (called once by the election module).
     */
    void vandal_set_role(vandal_role_t role);

    /**
     * @brief Check if this board is the master.
     */
    static inline bool vandal_is_master(void) { return vandal_get_role() == VANDAL_ROLE_MASTER; }

    /* ---------------------------------------------------------------------------
     * Protocol identifiers
     * --------------------------------------------------------------------------- */
    typedef enum
    {
        VANDAL_PROTO_UART,
        VANDAL_PROTO_I2C,
        VANDAL_PROTO_SPI,
        VANDAL_PROTO_ESPNOW,
        VANDAL_PROTO_BLE_OPEN,
        VANDAL_PROTO_BLE_AUTH,
        VANDAL_PROTO_THREAD,
        VANDAL_PROTO_HTTP,
        VANDAL_PROTO_MAX
    } vandal_proto_t;

    /* ---------------------------------------------------------------------------
     * Event system
     *
     * Every protocol module posts events to this custom event base.
     * The event_id carries the vandal_proto_t value, so the central logger
     * can discriminate the source protocol.
     * --------------------------------------------------------------------------- */
    ESP_EVENT_DECLARE_BASE(VANDAL_EVENT);

/* Event IDs (= protocol that received data) */
#define VANDAL_EVT_PAYLOAD_RECEIVED 0 /* event_data is vandal_event_payload_t* */

    typedef struct
    {
        vandal_proto_t protocol;
        char payload[128];
        size_t payload_len;
    } vandal_event_payload_t;

    /* ---------------------------------------------------------------------------
     * Payload helpers
     * --------------------------------------------------------------------------- */

    /**
     * @brief Initialize PRNG seed (call once at boot).
     */
    void vandal_payload_init(void);

    /**
     * @brief Build a payload string for a given protocol.
     *
     * If a custom payload is set for the protocol, it is used verbatim.
     * Otherwise a default string is generated:
     *   "VANDAL payload #42 sent through UART"
     *
     * @param proto  Protocol identifier.
     * @param buf    Destination buffer.
     * @param len    Size of destination buffer.
     * @return       Number of characters written (excluding NUL).
     */
    int vandal_payload_build(vandal_proto_t proto, char *buf, size_t len);

    /**
     * @brief Return a human-readable string for a protocol id.
     */
    const char *vandal_proto_name(vandal_proto_t proto);

    /**
     * @brief Resolve a protocol name string to its enum value.
     * @return protocol id, or VANDAL_PROTO_MAX if not found.
     */
    vandal_proto_t vandal_proto_from_name(const char *name);

    /* ---------------------------------------------------------------------------
     * Protocol runtime state (master-side on-demand start/stop)
     * --------------------------------------------------------------------------- */

    /**
     * @brief Check whether a protocol is currently running (TX enabled).
     */
    bool vandal_proto_is_running(vandal_proto_t proto);

    /**
     * @brief Set the running state of a protocol.
     */
    void vandal_proto_set_running(vandal_proto_t proto, bool running);

    /* ---------------------------------------------------------------------------
     * Custom payload per protocol
     * --------------------------------------------------------------------------- */

    /**
     * @brief Set a custom payload string for a protocol.
     *        Pass NULL or empty string to clear and revert to default.
     */
    void vandal_proto_set_custom_payload(vandal_proto_t proto, const char *payload);

    /**
     * @brief Get the custom payload string for a protocol (NULL if not set).
     */
    const char *vandal_proto_get_custom_payload(vandal_proto_t proto);

#ifdef __cplusplus
}
#endif
