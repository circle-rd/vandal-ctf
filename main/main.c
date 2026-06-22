/*
 * Vandal Test - Main entry point.
 *
 * Unified image: role is elected at boot via ESP-NOW broadcast.
 * The first board to boot (or highest random priority) becomes master.
 *
 * MASTER: boots with WiFi AP + HTTP server only.
 *         Protocols are started/stopped on demand via the web UI.
 *
 * SLAVE:  boots all receiving modules immediately.
 *         A central event logger subscribes to all protocol events.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#include "vandal_common.h"
#include "vandal_election.h"
#include "vandal_wifi.h"
#include "vandal_http.h"
#include "vandal_uart.h"
#include "vandal_i2c.h"
#include "vandal_spi.h"
#include "vandal_espnow.h"
#include "vandal_ble.h"
#include "vandal_thread.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

static const char *TAG = "vandal_main";

/* ── Heartbeat LED blink ─────────────────────────────────────────────────── */

static void heartbeat_task(void *arg)
{
    gpio_reset_pin(CONFIG_VANDAL_LED_PIN);
    gpio_set_direction(CONFIG_VANDAL_LED_PIN, GPIO_MODE_OUTPUT);

    /* Master: fast blink (250ms). Slave: slow blink (1000ms) */
    const TickType_t delay = vandal_is_master()
                                 ? pdMS_TO_TICKS(250)
                                 : pdMS_TO_TICKS(1000);

    bool state = false;
    while (1)
    {
        state = !state;
        gpio_set_level(CONFIG_VANDAL_LED_PIN, state);
        vTaskDelay(delay);
    }
}

/* ── Central event logger (slave only) ───────────────────────────────────── */

static void vandal_event_handler(void *handler_arg,
                                 esp_event_base_t base,
                                 int32_t id,
                                 void *event_data)
{
    if (base != VANDAL_EVENT || id != VANDAL_EVT_PAYLOAD_RECEIVED)
        return;

    const vandal_event_payload_t *evt = (const vandal_event_payload_t *)event_data;
    ESP_LOGI(TAG, "[%s] RX: %s", vandal_proto_name(evt->protocol), evt->payload);
}

/* ── app_main ────────────────────────────────────────────────────────────── */

void app_main(void)
{
    /* Initialize the pseudo-random payload tag */
    vandal_payload_init();

    ESP_LOGI(TAG, "==============================");
    ESP_LOGI(TAG, "  VANDAL CTF  — ELECTING...   ");
    ESP_LOGI(TAG, "==============================");

    /* ── Phase 1: WiFi STA for election ──────────────────────────────── */
    vandal_wifi_init_pre_election();

    /* ── Election: determine role at runtime ─────────────────────────── */
    vandal_role_t role = vandal_election_run();

    ESP_LOGI(TAG, "==============================");
    if (role == VANDAL_ROLE_MASTER)
    {
        ESP_LOGI(TAG, "  VANDAL CTF  — MASTER MODE   ");
    }
    else
    {
        ESP_LOGI(TAG, "  VANDAL CTF  — SLAVE MODE    ");
    }
    ESP_LOGI(TAG, "==============================");

    /* ── Phase 2: Reconfigure WiFi for elected role ──────────────────── */
    vandal_wifi_init_post_election(role);

    /* ── SLAVE: init all receiving modules immediately ───────────────── */
    if (role == VANDAL_ROLE_SLAVE)
    {
        vandal_espnow_init();

        /* Register the central logger for ALL protocol events */
        ESP_ERROR_CHECK(esp_event_handler_register(
            VANDAL_EVENT, VANDAL_EVT_PAYLOAD_RECEIVED,
            vandal_event_handler, NULL));

        vandal_uart_init();
        vandal_i2c_init();
        vandal_spi_init();
        vandal_thread_init();
    }

    /* ── BLE: initialized on both roles ──────────────────────────────── */
    /* Master: GATT server advertising open + auth services.              */
    /* Slave:  GATT client that connects and writes payloads.             */
    vandal_ble_init();

    /* ── MASTER: mount website FATFS partition ────────────────────────── */
    if (role == VANDAL_ROLE_MASTER)
    {
        static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
        const esp_vfs_fat_mount_config_t fat_cfg = {
            .max_files = 5,
            .format_if_mount_failed = false,
            .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        };
        esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(
            "/website", "website", &fat_cfg, &s_wl_handle);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to mount website FATFS: %s", esp_err_to_name(err));
        }
        else
        {
            ESP_LOGI(TAG, "Website FATFS mounted at /website");
        }
    }

    /* ── HTTP (server on master, client on slave) ─────────────────────── */
    vandal_http_init();

    /* ── Heartbeat LED ───────────────────────────────────────────────── */
    xTaskCreate(heartbeat_task, "heartbeat", 2048, NULL, 1, NULL);

    if (role == VANDAL_ROLE_MASTER)
    {
        ESP_LOGI(TAG, "AP + HTTP server ready. Use web UI to start protocols.");
    }
    else
    {
        ESP_LOGI(TAG, "All modules started. Running...");
    }
}
