/*
 * Vandal Test - Election module implementation.
 *
 * Uses ESP-NOW broadcast on the WiFi STA interface for a Bully-style
 * election protocol. The board with the highest (election_id, MAC) tuple
 * becomes master.
 */

#include "vandal_election.h"
#include "vandal_common.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "vandal_election";

/* ---------------------------------------------------------------------------
 * Configuration
 * --------------------------------------------------------------------------- */

#define ELECTION_CLAIM_INTERVAL_MS 200
#define ELECTION_RESULT_COUNT 5 /* how many times to broadcast result */

/* ---------------------------------------------------------------------------
 * Protocol messages
 * --------------------------------------------------------------------------- */

#define ELECTION_MSG_CLAIM 0x01
#define ELECTION_MSG_RESULT 0x03

typedef struct __attribute__((packed))
{
    uint8_t msg_type;
    uint32_t election_id;
    uint8_t mac[6];
} election_claim_t;

typedef struct __attribute__((packed))
{
    uint8_t msg_type;
    uint8_t master_mac[6];
    uint32_t master_id;
} election_result_t;

/* Magic prefix to distinguish election frames from other ESP-NOW traffic */
#define ELECTION_MAGIC 0x56 /* 'V' for Vandal */
#define ELECTION_MAGIC_OFF 0
#define ELECTION_DATA_OFF 1

/* ---------------------------------------------------------------------------
 * State
 * --------------------------------------------------------------------------- */

static uint32_t s_my_id = 0;
static uint8_t s_my_mac[6] = {0};

/* Event bits */
#define EVT_YIELDED BIT0
#define EVT_WON BIT1
#define EVT_GOT_RESULT BIT2

static EventGroupHandle_t s_election_events = NULL;

/* Broadcast peer */
static const uint8_t s_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* Forward declaration */
static void election_recv_cb(const esp_now_recv_info_t *recv_info,
                             const uint8_t *data, int data_len);

/* ---------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------------- */

/* Compare (id, mac) tuples. Returns >0 if (id_a, mac_a) > (id_b, mac_b) */
static int compare_priority(uint32_t id_a, const uint8_t *mac_a,
                            uint32_t id_b, const uint8_t *mac_b)
{
    if (id_a != id_b)
    {
        return (id_a > id_b) ? 1 : -1;
    }
    return memcmp(mac_a, mac_b, 6);
}

/* ---------------------------------------------------------------------------
 * ESP-NOW receive callback (runs in WiFi task context)
 * --------------------------------------------------------------------------- */

static void election_recv_cb(const esp_now_recv_info_t *recv_info,
                             const uint8_t *data, int data_len)
{
    if (data_len < 2)
        return;
    if (data[ELECTION_MAGIC_OFF] != ELECTION_MAGIC)
        return;

    const uint8_t *payload = data + ELECTION_DATA_OFF;
    int payload_len = data_len - ELECTION_DATA_OFF;

    uint8_t msg_type = payload[0];

    switch (msg_type)
    {
    case ELECTION_MSG_CLAIM:
    {
        if (payload_len < (int)sizeof(election_claim_t))
            return;
        const election_claim_t *claim = (const election_claim_t *)payload;

        /* Ignore our own claims (echoed back on some configs) */
        if (memcmp(claim->mac, s_my_mac, 6) == 0)
            return;

        int cmp = compare_priority(claim->election_id, claim->mac,
                                   s_my_id, s_my_mac);
        if (cmp > 0)
        {
            /* They have higher priority → we yield */
            ESP_LOGI(TAG, "Yielding to %02x:%02x:%02x:%02x:%02x:%02x (id=%lu)",
                     MAC2STR(claim->mac), (unsigned long)claim->election_id);
            xEventGroupSetBits(s_election_events, EVT_YIELDED);
        }
        break;
    }

    case ELECTION_MSG_RESULT:
    {
        if (payload_len < (int)sizeof(election_result_t))
            return;
        const election_result_t *result = (const election_result_t *)payload;

        /* If we're the master in the result, we won */
        if (memcmp(result->master_mac, s_my_mac, 6) == 0)
        {
            xEventGroupSetBits(s_election_events, EVT_WON);
        }
        else
        {
            /* Someone else is master → we're slave */
            ESP_LOGI(TAG, "Master announced: %02x:%02x:%02x:%02x:%02x:%02x",
                     MAC2STR(result->master_mac));
            xEventGroupSetBits(s_election_events, EVT_GOT_RESULT);
        }
        break;
    }

    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Send helpers
 * --------------------------------------------------------------------------- */

static void send_claim(void)
{
    uint8_t buf[1 + sizeof(election_claim_t)];
    buf[ELECTION_MAGIC_OFF] = ELECTION_MAGIC;

    election_claim_t *claim = (election_claim_t *)(buf + ELECTION_DATA_OFF);
    claim->msg_type = ELECTION_MSG_CLAIM;
    claim->election_id = s_my_id;
    memcpy(claim->mac, s_my_mac, 6);

    esp_now_send(s_broadcast_mac, buf, sizeof(buf));
}

static void send_result(void)
{
    uint8_t buf[1 + sizeof(election_result_t)];
    buf[ELECTION_MAGIC_OFF] = ELECTION_MAGIC;

    election_result_t *result = (election_result_t *)(buf + ELECTION_DATA_OFF);
    result->msg_type = ELECTION_MSG_RESULT;
    memcpy(result->master_mac, s_my_mac, 6);
    result->master_id = s_my_id;

    esp_now_send(s_broadcast_mac, buf, sizeof(buf));
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

vandal_role_t vandal_election_run(void)
{
    /* Get our MAC */
    esp_read_mac(s_my_mac, ESP_MAC_WIFI_STA);

    /* Generate random election priority */
    s_my_id = esp_random();

    ESP_LOGI(TAG, "Election started: id=%lu, MAC=%02x:%02x:%02x:%02x:%02x:%02x",
             (unsigned long)s_my_id, MAC2STR(s_my_mac));

    /* Create event group */
    s_election_events = xEventGroupCreate();

    /* Initialize ESP-NOW for election */
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(election_recv_cb));

    /* Add broadcast peer */
    esp_now_peer_info_t peer = {
        .channel = 0, /* use current channel */
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, s_broadcast_mac, 6);
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));

    /* ── Election loop: broadcast claims, watch for yields ─────────── */
    vandal_role_t role = VANDAL_ROLE_MASTER; /* optimistic */
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(CONFIG_VANDAL_ELECTION_TIMEOUT_MS);

    while (1)
    {
        /* Check if we've been pre-empted */
        EventBits_t bits = xEventGroupGetBits(s_election_events);
        if (bits & EVT_YIELDED)
        {
            role = VANDAL_ROLE_SLAVE;
            break;
        }
        if (bits & EVT_GOT_RESULT)
        {
            role = VANDAL_ROLE_SLAVE;
            break;
        }

        /* Check timeout */
        TickType_t elapsed = xTaskGetTickCount() - start_tick;
        if (elapsed >= timeout_ticks)
        {
            /* No one beat us → we win */
            role = VANDAL_ROLE_MASTER;
            break;
        }

        /* Broadcast our claim */
        send_claim();

        /* Wait a bit, but wake early if yielded */
        xEventGroupWaitBits(s_election_events,
                            EVT_YIELDED | EVT_GOT_RESULT,
                            pdFALSE, pdFALSE,
                            pdMS_TO_TICKS(ELECTION_CLAIM_INTERVAL_MS));
    }

    /* ── Post-election ─────────────────────────────────────────────── */
    if (role == VANDAL_ROLE_MASTER)
    {
        /* Announce ourselves as master multiple times for reliability */
        for (int i = 0; i < ELECTION_RESULT_COUNT; i++)
        {
            send_result();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        ESP_LOGI(TAG, "*** ELECTED MASTER ***");
    }
    else
    {
        /* Wait briefly for the RESULT message (may already have it) */
        xEventGroupWaitBits(s_election_events, EVT_GOT_RESULT,
                            pdFALSE, pdFALSE, pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "*** ELECTED SLAVE ***");
    }

    /* ── Cleanup ESP-NOW (will be re-initialized by protocol module) ── */
    esp_now_unregister_recv_cb();
    esp_now_del_peer(s_broadcast_mac);
    esp_now_deinit();

    vEventGroupDelete(s_election_events);
    s_election_events = NULL;

    /* Set the role globally */
    vandal_set_role(role);
    return role;
}
