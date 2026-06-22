/*
 * Vandal Test - Common module implementation.
 */

#include "vandal_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_random.h"

/* Register the custom event base */
ESP_EVENT_DEFINE_BASE(VANDAL_EVENT);

/* Board role (set once by election, read many times) */
static vandal_role_t s_role = VANDAL_ROLE_SLAVE; /* safe default */

/* Pseudo-random tag generated once at boot */
static uint32_t s_payload_tag = 0;

/* Protocol running state (master TX gating) */
static bool s_proto_running[VANDAL_PROTO_MAX] = {false};

/* Custom payloads per protocol (empty string = not set) */
static char s_custom_payloads[VANDAL_PROTO_MAX][128] = {{0}};

static const char *s_proto_names[] = {
    [VANDAL_PROTO_UART] = "UART",
    [VANDAL_PROTO_I2C] = "I2C",
    [VANDAL_PROTO_SPI] = "SPI",
    [VANDAL_PROTO_ESPNOW] = "ESP-NOW",
    [VANDAL_PROTO_BLE_OPEN] = "BLE-OPEN",
    [VANDAL_PROTO_BLE_AUTH] = "BLE-AUTH",
    [VANDAL_PROTO_THREAD] = "Thread",
    [VANDAL_PROTO_HTTP] = "HTTP",
};

void vandal_payload_init(void)
{
    s_payload_tag = esp_random() % 10000; /* 0 – 9999 */
}

/* ── Role management ─────────────────────────────────────────────────────── */

vandal_role_t vandal_get_role(void)
{
    return s_role;
}

void vandal_set_role(vandal_role_t role)
{
    s_role = role;
}

int vandal_payload_build(vandal_proto_t proto, char *buf, size_t len)
{
    /* Use custom payload if one is set */
    const char *custom = vandal_proto_get_custom_payload(proto);
    if (custom)
    {
        return snprintf(buf, len, "%s", custom);
    }
    return snprintf(buf, len, "VANDAL payload #%04lu sent through %s",
                    (unsigned long)s_payload_tag, vandal_proto_name(proto));
}

const char *vandal_proto_name(vandal_proto_t proto)
{
    if (proto >= VANDAL_PROTO_MAX)
    {
        return "UNKNOWN";
    }
    return s_proto_names[proto];
}

vandal_proto_t vandal_proto_from_name(const char *name)
{
    if (name == NULL)
        return VANDAL_PROTO_MAX;
    for (int i = 0; i < VANDAL_PROTO_MAX; i++)
    {
        if (strcmp(name, s_proto_names[i]) == 0)
        {
            return (vandal_proto_t)i;
        }
    }
    return VANDAL_PROTO_MAX;
}

/* ── Runtime state ───────────────────────────────────────────────────────── */

bool vandal_proto_is_running(vandal_proto_t proto)
{
    if (proto >= VANDAL_PROTO_MAX)
        return false;
    return s_proto_running[proto];
}

void vandal_proto_set_running(vandal_proto_t proto, bool running)
{
    if (proto >= VANDAL_PROTO_MAX)
        return;
    s_proto_running[proto] = running;
}

/* ── Custom payloads ─────────────────────────────────────────────────────── */

void vandal_proto_set_custom_payload(vandal_proto_t proto, const char *payload)
{
    if (proto >= VANDAL_PROTO_MAX)
        return;
    if (payload == NULL || payload[0] == '\0')
    {
        s_custom_payloads[proto][0] = '\0';
    }
    else
    {
        strncpy(s_custom_payloads[proto], payload, sizeof(s_custom_payloads[0]) - 1);
        s_custom_payloads[proto][sizeof(s_custom_payloads[0]) - 1] = '\0';
    }
}

const char *vandal_proto_get_custom_payload(vandal_proto_t proto)
{
    if (proto >= VANDAL_PROTO_MAX)
        return NULL;
    if (s_custom_payloads[proto][0] == '\0')
        return NULL;
    return s_custom_payloads[proto];
}
