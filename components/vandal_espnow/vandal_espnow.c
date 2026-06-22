/*
 * Vandal Test - ESP-NOW module implementation.
 *
 * Master: sends payload to broadcast address periodically.
 * Slave:  receives data and forwards it to the event bus.
 *
 * Role determined at runtime via vandal_get_role().
 */

#include "vandal_espnow.h"
#include "vandal_common.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "sdkconfig.h"

static const char *TAG = "vandal_espnow";

/* Broadcast MAC address (master TX) */
static const uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* ---------------------------------------------------------------------------
 * Callbacks
 * --------------------------------------------------------------------------- */

static void espnow_send_cb(const wifi_tx_info_t *tx_info, esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS)
    {
        ESP_LOGW(TAG, "ESP-NOW send failed (status=%d)", status);
    }
}

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                           const uint8_t *data, int data_len)
{
    if (data_len <= 0)
        return;

    vandal_event_payload_t evt = {
        .protocol = VANDAL_PROTO_ESPNOW,
        .payload_len = (size_t)data_len,
    };
    size_t copy_len = data_len;
    if (copy_len >= sizeof(evt.payload))
    {
        copy_len = sizeof(evt.payload) - 1;
    }
    memcpy(evt.payload, data, copy_len);
    evt.payload[copy_len] = '\0';

    esp_event_post(VANDAL_EVENT, VANDAL_EVT_PAYLOAD_RECEIVED,
                   &evt, sizeof(evt), portMAX_DELAY);
}

/* ---------------------------------------------------------------------------
 * Master task
 * --------------------------------------------------------------------------- */

static bool s_hw_init = false;
static TaskHandle_t s_task_handle = NULL;

static void espnow_master_task(void *arg)
{
    char payload[128];
    while (1)
    {
        if (vandal_proto_is_running(VANDAL_PROTO_ESPNOW))
        {
            int len = vandal_payload_build(VANDAL_PROTO_ESPNOW, payload, sizeof(payload));
            esp_err_t err = esp_now_send(s_broadcast_mac, (const uint8_t *)payload, len);
            if (err == ESP_OK)
            {
                ESP_LOGI(TAG, "TX: %s", payload);
            }
            else
            {
                ESP_LOGW(TAG, "TX failed: %s", esp_err_to_name(err));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_VANDAL_PAYLOAD_SEND_INTERVAL_MS));
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

void vandal_espnow_init(void)
{
    if (vandal_is_master())
    {
        /* Master: no-op at init — lazy init on start() */
        return;
    }

    /* Slave: init immediately and register receive callback */
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));
    ESP_LOGI(TAG, "Slave initialized on channel %d", CONFIG_VANDAL_ESPNOW_CHANNEL);
}

void vandal_espnow_start(void)
{
    if (!vandal_is_master())
        return; /* no-op on slave */

    if (!s_hw_init)
    {
        ESP_ERROR_CHECK(esp_now_init());
        ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));

        esp_now_peer_info_t peer = {
            .channel = CONFIG_VANDAL_ESPNOW_CHANNEL,
            .ifidx = WIFI_IF_AP,
            .encrypt = false,
        };
        memcpy(peer.peer_addr, s_broadcast_mac, ESP_NOW_ETH_ALEN);
        ESP_ERROR_CHECK(esp_now_add_peer(&peer));

        ESP_LOGI(TAG, "Master initialized on channel %d", CONFIG_VANDAL_ESPNOW_CHANNEL);
        s_hw_init = true;
        xTaskCreate(espnow_master_task, "espnow_master", 4096, NULL, 5, &s_task_handle);
    }
    vandal_proto_set_running(VANDAL_PROTO_ESPNOW, true);
    ESP_LOGI(TAG, "Started");
}

void vandal_espnow_stop(void)
{
    if (!vandal_is_master())
        return; /* no-op on slave */

    vandal_proto_set_running(VANDAL_PROTO_ESPNOW, false);
    ESP_LOGI(TAG, "Stopped");
}
