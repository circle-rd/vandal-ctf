/*
 * Vandal Test - WiFi module implementation.
 *
 * Two-phase initialization:
 *   Phase 1 (pre-election): STA mode on fixed channel — just enough for ESP-NOW.
 *   Phase 2 (post-election): Master switches to AP, Slave connects STA to master.
 */

#include "vandal_wifi.h"
#include "vandal_common.h"

#include <string.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "vandal_wifi";

/* Track whether STA auto-connect is enabled (only after post-election for slave) */
static bool s_sta_auto_connect = false;

/* WiFi event handler */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_AP_STACONNECTED:
        {
            wifi_event_ap_staconnected_t *e = event_data;
            ESP_LOGI(TAG, "Station %02x:%02x:%02x:%02x:%02x:%02x joined (AID=%d)",
                     MAC2STR(e->mac), e->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED:
        {
            wifi_event_ap_stadisconnected_t *e = event_data;
            ESP_LOGI(TAG, "Station %02x:%02x:%02x:%02x:%02x:%02x left (AID=%d)",
                     MAC2STR(e->mac), e->aid);
            break;
        }
        case WIFI_EVENT_STA_START:
            if (s_sta_auto_connect)
            {
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (s_sta_auto_connect)
            {
                ESP_LOGW(TAG, "Disconnected from master AP, reconnecting...");
                esp_wifi_connect();
            }
            break;
        default:
            break;
        }
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected to master AP, IP: " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

/* ── Phase 1: STA mode for election ─────────────────────────────────────── */

void vandal_wifi_init_pre_election(void)
{
    /* NVS is required by WiFi driver */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Create both netifs upfront (AP netif needed later if master) */
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    /* Start in STA mode on the election channel (no connect yet) */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t sta_cfg = {
        .sta = {
            .channel = CONFIG_VANDAL_WIFI_AP_CHANNEL,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Phase 1: STA started on channel %d (for election)",
             CONFIG_VANDAL_WIFI_AP_CHANNEL);
}

/* ── Phase 2: Reconfigure based on role ──────────────────────────────────── */

/* Helper: map KConfig choice to wifi_auth_mode_t */
static wifi_auth_mode_t configured_auth_mode(void)
{
#if defined(CONFIG_VANDAL_WIFI_AP_AUTH_WPA_PSK)
    return WIFI_AUTH_WPA_PSK;
#elif defined(CONFIG_VANDAL_WIFI_AP_AUTH_WPA2_PSK)
    return WIFI_AUTH_WPA2_PSK;
#elif defined(CONFIG_VANDAL_WIFI_AP_AUTH_WPA_WPA2_PSK)
    return WIFI_AUTH_WPA_WPA2_PSK;
#elif defined(CONFIG_VANDAL_WIFI_AP_AUTH_WPA3_PSK)
    return WIFI_AUTH_WPA3_PSK;
#elif defined(CONFIG_VANDAL_WIFI_AP_AUTH_WPA2_WPA3_PSK)
    return WIFI_AUTH_WPA2_WPA3_PSK;
#else
    return WIFI_AUTH_OPEN;
#endif
}

void vandal_wifi_init_post_election(vandal_role_t role)
{
    wifi_auth_mode_t authmode = configured_auth_mode();

    if (role == VANDAL_ROLE_MASTER)
    {
        /* Stop STA, switch to AP mode */
        ESP_ERROR_CHECK(esp_wifi_stop());
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

        wifi_config_t ap_cfg = {
            .ap = {
                .ssid = CONFIG_VANDAL_WIFI_AP_SSID,
                .ssid_len = strlen(CONFIG_VANDAL_WIFI_AP_SSID),
                .channel = CONFIG_VANDAL_WIFI_AP_CHANNEL,
                .max_connection = CONFIG_VANDAL_WIFI_AP_MAX_CONN,
                .authmode = authmode,
            },
        };

        /* Set password/key for non-OPEN modes */
        if (authmode != WIFI_AUTH_OPEN)
        {
            strncpy((char *)ap_cfg.ap.password, CONFIG_VANDAL_WIFI_AP_PASSWORD,
                    sizeof(ap_cfg.ap.password) - 1);
        }

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        ESP_ERROR_CHECK(esp_wifi_start());

        ESP_LOGI(TAG, "Phase 2: AP started: SSID=\"%s\", channel=%d, auth=%d",
                 CONFIG_VANDAL_WIFI_AP_SSID, CONFIG_VANDAL_WIFI_AP_CHANNEL, authmode);
    }
    else
    {
        /* Stay in STA mode, connect to master's AP */
        s_sta_auto_connect = true;

        wifi_config_t sta_cfg = {
            .sta = {
                .ssid = CONFIG_VANDAL_WIFI_AP_SSID,
                .threshold.authmode = authmode,
            },
        };

        /* Set password/key for non-OPEN modes */
        if (authmode != WIFI_AUTH_OPEN)
        {
            strncpy((char *)sta_cfg.sta.password, CONFIG_VANDAL_WIFI_AP_PASSWORD,
                    sizeof(sta_cfg.sta.password) - 1);
        }

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
        esp_wifi_connect();

        ESP_LOGI(TAG, "Phase 2: STA connecting to \"%s\" (channel=%d, auth=%d)",
                 CONFIG_VANDAL_WIFI_AP_SSID, CONFIG_VANDAL_WIFI_AP_CHANNEL, authmode);
    }
}
