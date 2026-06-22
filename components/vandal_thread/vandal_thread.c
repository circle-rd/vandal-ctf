/*
 * Vandal Test - Thread (OpenThread) module implementation.
 *
 * Uses the ESP-IDF OpenThread integration with native 802.15.4 radio.
 * Master: FTD leader, periodically sends VANDAL payloads via UDP multicast.
 * Slave:  FTD child, receives UDP payloads and posts to the event bus.
 *
 * Both boards share the same Thread network credentials (Kconfig defaults)
 * so they auto-form / auto-join a single Thread network.
 * The network key is static and well-known — vulnerable by design.
 */

#include "vandal_thread.h"
#include "vandal_common.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_vfs_eventfd.h"
#include "sdkconfig.h"

#include "openthread/instance.h"
#include "openthread/thread.h"
#include "openthread/udp.h"
#include "openthread/message.h"
#include "openthread/ip6.h"
#include "openthread/dataset.h"
#include "openthread/link.h"

static const char *TAG = "vandal_thread";

/* Thread UDP port for our test payloads */
#define VANDAL_THREAD_PORT 12345

/* Mesh-local all-nodes multicast: ff03::1 */
static const otIp6Address s_multicast_addr = {
    .mFields.m8 = {0xff, 0x03, 0, 0, 0, 0, 0, 0,
                   0, 0, 0, 0, 0, 0, 0, 1}};

static otUdpSocket s_udp_socket;
static bool s_socket_open = false;

/* =========================================================================
 * UDP receive callback (called from OT main loop context)
 * ========================================================================= */

static void udp_receive_cb(void *aContext, otMessage *aMessage,
                           const otMessageInfo *aMessageInfo)
{
    (void)aContext;
    (void)aMessageInfo;

    uint16_t len = otMessageGetLength(aMessage) - otMessageGetOffset(aMessage);
    if (len == 0 || len >= 128)
        return;

    char buf[128];
    int read = otMessageRead(aMessage, otMessageGetOffset(aMessage), buf, len);
    if (read <= 0)
        return;
    buf[read] = '\0';

    ESP_LOGI(TAG, "RX (%d bytes): %s", read, buf);

    /* Post event to the Vandal event bus */
    vandal_event_payload_t evt = {
        .protocol = VANDAL_PROTO_THREAD,
        .payload_len = (size_t)read,
    };
    strncpy(evt.payload, buf, sizeof(evt.payload) - 1);
    evt.payload[sizeof(evt.payload) - 1] = '\0';

    esp_event_post(VANDAL_EVENT, VANDAL_EVT_PAYLOAD_RECEIVED,
                   &evt, sizeof(evt), pdMS_TO_TICKS(100));
}

/* =========================================================================
 * Open the UDP socket (must hold OT lock)
 * ========================================================================= */

static void open_udp_socket(otInstance *instance)
{
    if (s_socket_open)
        return;

    memset(&s_udp_socket, 0, sizeof(s_udp_socket));

    otError err = otUdpOpen(instance, &s_udp_socket, udp_receive_cb, NULL);
    if (err != OT_ERROR_NONE)
    {
        ESP_LOGE(TAG, "otUdpOpen failed: %d", err);
        return;
    }

    otSockAddr bindAddr;
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.mPort = VANDAL_THREAD_PORT;

    err = otUdpBind(instance, &s_udp_socket, &bindAddr, OT_NETIF_THREAD_INTERNAL);
    if (err != OT_ERROR_NONE)
    {
        ESP_LOGE(TAG, "otUdpBind failed: %d", err);
        return;
    }

    s_socket_open = true;
    ESP_LOGI(TAG, "UDP socket open on port %d", VANDAL_THREAD_PORT);
}

/* =========================================================================
 * Send a payload via UDP multicast (must hold OT lock)
 * ========================================================================= */

static void send_udp_payload(otInstance *instance)
{
    char payload[128];
    vandal_payload_build(VANDAL_PROTO_THREAD, payload, sizeof(payload));

    /* Self-report the TX payload to the event bus so the master's /status
     * endpoint reflects Thread activity even before the slave echoes it back. */
    vandal_event_payload_t self_evt = {
        .protocol = VANDAL_PROTO_THREAD,
        .payload_len = strlen(payload),
    };
    strncpy(self_evt.payload, payload, sizeof(self_evt.payload) - 1);
    self_evt.payload[sizeof(self_evt.payload) - 1] = '\0';
    esp_event_post(VANDAL_EVENT, VANDAL_EVT_PAYLOAD_RECEIVED,
                   &self_evt, sizeof(self_evt), pdMS_TO_TICKS(100));

    otMessage *msg = otUdpNewMessage(instance, NULL);
    if (msg == NULL)
    {
        ESP_LOGW(TAG, "otUdpNewMessage failed");
        return;
    }

    otError err = otMessageAppend(msg, payload, (uint16_t)strlen(payload));
    if (err != OT_ERROR_NONE)
    {
        ESP_LOGW(TAG, "otMessageAppend failed: %d", err);
        otMessageFree(msg);
        return;
    }

    otMessageInfo msgInfo;
    memset(&msgInfo, 0, sizeof(msgInfo));
    msgInfo.mPeerAddr = s_multicast_addr;
    msgInfo.mPeerPort = VANDAL_THREAD_PORT;

    err = otUdpSend(instance, &s_udp_socket, msg, &msgInfo);
    if (err != OT_ERROR_NONE)
    {
        ESP_LOGW(TAG, "otUdpSend failed: %d", err);
        /* msg is freed by OT on failure? No — only on success */
        otMessageFree(msg);
    }
}

/* =========================================================================
 * State change callback — log role transitions
 * ========================================================================= */

static void ot_state_changed_cb(otChangedFlags aFlags, void *aContext)
{
    otInstance *instance = (otInstance *)aContext;

    if (aFlags & OT_CHANGED_THREAD_ROLE)
    {
        otDeviceRole role = otThreadGetDeviceRole(instance);
        const char *role_str;
        switch (role)
        {
        case OT_DEVICE_ROLE_DISABLED:
            role_str = "Disabled";
            break;
        case OT_DEVICE_ROLE_DETACHED:
            role_str = "Detached";
            break;
        case OT_DEVICE_ROLE_CHILD:
            role_str = "Child";
            break;
        case OT_DEVICE_ROLE_ROUTER:
            role_str = "Router";
            break;
        case OT_DEVICE_ROLE_LEADER:
            role_str = "Leader";
            break;
        default:
            role_str = "Unknown";
            break;
        }
        ESP_LOGI(TAG, "Role changed → %s", role_str);

        /* Open the UDP socket as soon as we have a valid role */
        if (role >= OT_DEVICE_ROLE_CHILD && !s_socket_open)
        {
            open_udp_socket(instance);
        }
    }
}

/* =========================================================================
 * Master TX task — sends payloads periodically
 * ========================================================================= */

static void thread_tx_task(void *arg)
{
    /* Wait for Thread network to form */
    vTaskDelay(pdMS_TO_TICKS(8000));

    while (1)
    {
        if (vandal_proto_is_running(VANDAL_PROTO_THREAD))
        {
            otInstance *instance = esp_openthread_get_instance();
            if (instance)
            {
                esp_openthread_lock_acquire(portMAX_DELAY);
                otDeviceRole role = otThreadGetDeviceRole(instance);
                if (role >= OT_DEVICE_ROLE_CHILD)
                {
                    if (!s_socket_open)
                    {
                        open_udp_socket(instance);
                    }
                    if (s_socket_open)
                    {
                        send_udp_payload(instance);
                    }
                }
                esp_openthread_lock_release();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

/* =========================================================================
 * Internal: full OpenThread initialization (called once, lazily)
 * ========================================================================= */

static bool s_ot_init = false;
static bool s_ot_starting = false;
static TaskHandle_t s_tx_task = NULL;

static void thread_do_init(void)
{
    if (s_ot_init)
        return;

    /* eventfd is required by OpenThread platform layer */
    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = 3,
    };
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

    /* OpenThread configuration: native 802.15.4 radio, no host RCP */
    static esp_openthread_config_t config = {
        .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
        .platform_config = {
            .radio_config = {
                .radio_mode = RADIO_MODE_NATIVE,
            },
            .host_config = {
                .host_connection_mode = HOST_CONNECTION_MODE_NONE,
            },
            .port_config = {
                .storage_partition_name = "nvs",
                .netif_queue_size = 10,
                .task_queue_size = 10,
            },
        },
    };

    /* Start the OpenThread task (init + mainloop handled internally) */
    ESP_ERROR_CHECK(esp_openthread_start(&config));

    /* Wait briefly for OT to start, then configure */
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *instance = esp_openthread_get_instance();

    /* Register state change callback */
    otSetStateChangedCallback(instance, ot_state_changed_cb, instance);

    /* Set the operational dataset from Kconfig defaults and auto-start */
    ESP_ERROR_CHECK(esp_openthread_auto_start(NULL));

    esp_openthread_lock_release();

    s_ot_init = true;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

/* One-shot task: performs the blocking OT init outside the HTTPD context */
static void thread_launch_task(void *arg)
{
    thread_do_init();
    xTaskCreate(thread_tx_task, "thread_tx", 4096, NULL, 5, &s_tx_task);
    ESP_LOGI(TAG, "Master: FTD leader, TX on port %d (multicast ff03::1)",
             VANDAL_THREAD_PORT);
    vTaskDelete(NULL);
}

void vandal_thread_init(void)
{
    if (vandal_is_master())
        return; /* master uses lazy init via start() */

    thread_do_init();
    ESP_LOGI(TAG, "Slave: FTD child, RX on port %d", VANDAL_THREAD_PORT);
}

void vandal_thread_start(void)
{
    if (!vandal_is_master())
        return; /* no-op on slave */

    vandal_proto_set_running(VANDAL_PROTO_THREAD, true);
    if (!s_ot_init && !s_ot_starting)
    {
        s_ot_starting = true;
        xTaskCreate(thread_launch_task, "thread_init", 6144, NULL, 5, NULL);
    }
    ESP_LOGI(TAG, "Started");
}

void vandal_thread_stop(void)
{
    if (!vandal_is_master())
        return; /* no-op on slave */

    vandal_proto_set_running(VANDAL_PROTO_THREAD, false);
    ESP_LOGI(TAG, "Stopped");
}
