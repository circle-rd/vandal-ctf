/*
 * Vandal Test - BLE module implementation using NimBLE.
 *
 * Master = GATT Server (peripheral) that advertises two services:
 *   - Open service:  one characteristic, readable without pairing.
 *   - Auth service:  one characteristic, requires PIN-based pairing.
 *                    (vulnerable by design — static PIN for brute-force testing).
 *
 * Slave = GATT Client (central) that scans, connects, discovers services,
 *         pairs when required, reads characteristics and posts values as events.
 *
 * Role determined at runtime via vandal_get_role().
 */

#include "vandal_ble.h"
#include "vandal_common.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

/* NimBLE headers */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "host/ble_gap.h"

static const char *TAG = "vandal_ble";

/* ── UUIDs (derived from KConfig strings) ────────────────────────────────── */

static ble_uuid128_t s_open_svc_uuid;
static ble_uuid128_t s_open_chr_uuid;
static ble_uuid128_t s_auth_svc_uuid;
static ble_uuid128_t s_auth_chr_uuid;

static void uuid_from_str(const char *str, ble_uuid128_t *out)
{
    out->u.type = BLE_UUID_TYPE_128;
    uint8_t bytes[16];
    int idx = 0;
    for (int i = 0; str[i] && idx < 16; i++)
    {
        if (str[i] == '-')
            continue;
        unsigned int val = 0;
        sscanf(&str[i], "%2x", &val);
        bytes[idx++] = (uint8_t)val;
        i++;
    }
    for (int i = 0; i < 16; i++)
    {
        out->value[15 - i] = bytes[i];
    }
}

static void derive_chr_uuid(const ble_uuid128_t *svc, ble_uuid128_t *chr)
{
    *chr = *svc;
    chr->value[13] = 0x01;
}

/* =========================================================================
 * MASTER — GATT Server (Peripheral)
 * ========================================================================= */

static char s_open_payload[128];
static char s_auth_payload[128];
static uint16_t s_open_chr_handle;
static uint16_t s_auth_chr_handle;

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    char *dest = NULL;
    vandal_proto_t proto = VANDAL_PROTO_MAX;

    if (attr_handle == s_open_chr_handle)
    {
        dest  = s_open_payload;
        proto = VANDAL_PROTO_BLE_OPEN;
    }
    else if (attr_handle == s_auth_chr_handle)
    {
        dest  = s_auth_payload;
        proto = VANDAL_PROTO_BLE_AUTH;
    }

    if (dest == NULL)
        return BLE_ATT_ERR_UNLIKELY;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR)
    {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len >= sizeof(s_open_payload))
            len = sizeof(s_open_payload) - 1;
        os_mbuf_copydata(ctxt->om, 0, len, dest);
        dest[len] = '\0';

        vandal_event_payload_t evt = {
            .protocol   = proto,
            .payload_len = len,
        };
        memcpy(evt.payload, dest, len + 1);
        esp_event_post(VANDAL_EVENT, VANDAL_EVT_PAYLOAD_RECEIVED,
                       &evt, sizeof(evt), pdMS_TO_TICKS(100));
        return 0;
    }

    /* READ */
    int rc = os_mbuf_append(ctxt->om, dest, strlen(dest));
    return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = (ble_uuid_t *)&s_open_svc_uuid,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = (ble_uuid_t *)&s_open_chr_uuid,
                .access_cb = gatt_access_cb,
                .val_handle = &s_open_chr_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            {0}
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = (ble_uuid_t *)&s_auth_svc_uuid,
        .characteristics = (struct ble_gatt_chr_def[]){{
                                                           .uuid = (ble_uuid_t *)&s_auth_chr_uuid,
                                                           .access_cb = gatt_access_cb,
                                                           .val_handle = &s_auth_chr_handle,
                                                           .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_AUTHEN,
                                                       },
                                                       {0}},
    },
    {0}
};

/* ── Master GAP event handler ────────────────────────────────────────────── */

static int master_ble_gap_event_cb(struct ble_gap_event *event, void *arg);

static void ble_advertise(void)
{
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)CONFIG_VANDAL_BLE_DEVICE_NAME;
    fields.name_len = strlen(CONFIG_VANDAL_BLE_DEVICE_NAME);
    fields.name_is_complete = 1;

    ble_gap_adv_set_fields(&fields);
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                      &adv_params, master_ble_gap_event_cb, NULL);
}

static int master_ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "Client connected (handle=%d, status=%d)",
                 event->connect.conn_handle, event->connect.status);
        if (event->connect.status != 0)
        {
            ble_advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Client disconnected (reason=%d)",
                 event->disconnect.reason);
        ble_advertise();
        break;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_DISP)
        {
            struct ble_sm_io pk = {
                .action = BLE_SM_IOACT_DISP,
                .passkey = CONFIG_VANDAL_BLE_PIN,
            };
            ble_sm_inject_io(event->passkey.conn_handle, &pk);
            ESP_LOGI(TAG, "Displaying passkey: %06d", (int)CONFIG_VANDAL_BLE_PIN);
        }
        break;

    default:
        break;
    }
    return 0;
}

/* ── Master NimBLE host sync callback ────────────────────────────────────── */

static void master_ble_on_sync(void)
{
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;

    ble_advertise();
    ESP_LOGI(TAG, "Server advertising as \"%s\"", CONFIG_VANDAL_BLE_DEVICE_NAME);
}

/* =========================================================================
 * SLAVE — GATT Client (Central)
 * ========================================================================= */

static uint16_t s_conn_handle;
static bool s_connected = false;

static uint16_t s_open_val_handle = 0;
static uint16_t s_auth_val_handle = 0;

static uint16_t s_disc_start = 0;
static uint16_t s_disc_end = 0;

static volatile bool s_cycle_busy = false;
static TickType_t s_cycle_start_tick = 0;

#define BLE_CYCLE_TIMEOUT_MS 8000

static void cli_start_open_discovery(void);
static void cli_start_auth_discovery(void);
static void cli_write_open(void);
static void cli_write_auth(void);

static int cli_write_cb(uint16_t conn_handle,
                        const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr,
                        void *arg)
{
    vandal_proto_t proto = (vandal_proto_t)(uintptr_t)arg;

    if (error->status == 0)
    {
        /* Write acknowledged — report the written payload via event bus */
        char payload[128];
        int len = vandal_payload_build(proto, payload, sizeof(payload));
        vandal_event_payload_t evt = {
            .protocol    = proto,
            .payload_len = (size_t)len,
        };
        strncpy(evt.payload, payload, sizeof(evt.payload) - 1);
        evt.payload[sizeof(evt.payload) - 1] = '\0';
        esp_event_post(VANDAL_EVENT, VANDAL_EVT_PAYLOAD_RECEIVED,
                       &evt, sizeof(evt), portMAX_DELAY);
    }
    else
    {
        if (proto == VANDAL_PROTO_BLE_AUTH && error->status == 261)
        {
            ESP_LOGI(TAG, "AUTH write requires pairing, initiating security...");
            ble_gap_security_initiate(conn_handle);
            return 0;
        }
        ESP_LOGW(TAG, "Write failed for %s (status=%d)",
                 vandal_proto_name(proto), error->status);
    }

    if (proto == VANDAL_PROTO_BLE_OPEN)
    {
        if (s_auth_val_handle != 0)
        {
            cli_write_auth();
        }
        else
        {
            cli_start_auth_discovery();
        }
    }
    else
    {
        s_cycle_busy = false;
    }
    return 0;
}

static int cli_disc_open_chr_cb(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0 && chr != NULL)
    {
        s_open_val_handle = chr->val_handle;
        ESP_LOGI(TAG, "Found OPEN characteristic (handle=%d)", chr->val_handle);
    }
    else if (error->status == BLE_HS_EDONE)
    {
        if (s_open_val_handle != 0)
        {
            cli_write_open();
        }
        else
        {
            ESP_LOGW(TAG, "OPEN characteristic not found, skipping to AUTH");
            cli_start_auth_discovery();
        }
    }
    else
    {
        ESP_LOGW(TAG, "OPEN chr discovery error: %d", error->status);
        cli_start_auth_discovery();
    }
    return 0;
}

static int cli_disc_auth_chr_cb(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                const struct ble_gatt_chr *chr, void *arg)
{
    if (error->status == 0 && chr != NULL)
    {
        s_auth_val_handle = chr->val_handle;
        ESP_LOGI(TAG, "Found AUTH characteristic (handle=%d)", chr->val_handle);
    }
    else if (error->status == BLE_HS_EDONE)
    {
        if (s_auth_val_handle != 0)
        {
            cli_write_auth();
        }
        else
        {
            ESP_LOGW(TAG, "AUTH characteristic not found");
            s_cycle_busy = false;
        }
    }
    else
    {
        ESP_LOGW(TAG, "AUTH chr discovery error: %d", error->status);
        s_cycle_busy = false;
    }
    return 0;
}

static int cli_disc_open_svc_cb(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                const struct ble_gatt_svc *svc, void *arg)
{
    if (error->status == 0 && svc != NULL)
    {
        s_disc_start = svc->start_handle;
        s_disc_end = svc->end_handle;
        ESP_LOGI(TAG, "Found OPEN service (handles %d-%d)",
                 svc->start_handle, svc->end_handle);
    }
    else if (error->status == BLE_HS_EDONE)
    {
        if (s_disc_start != 0)
        {
            uint16_t start = s_disc_start, end = s_disc_end;
            s_disc_start = 0;
            ble_gattc_disc_chrs_by_uuid(conn_handle, start, end,
                                        (ble_uuid_t *)&s_open_chr_uuid,
                                        cli_disc_open_chr_cb, NULL);
        }
        else
        {
            ESP_LOGW(TAG, "OPEN service not found, skipping to AUTH");
            cli_start_auth_discovery();
        }
    }
    return 0;
}

static int cli_disc_auth_svc_cb(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                const struct ble_gatt_svc *svc, void *arg)
{
    if (error->status == 0 && svc != NULL)
    {
        s_disc_start = svc->start_handle;
        s_disc_end = svc->end_handle;
        ESP_LOGI(TAG, "Found AUTH service (handles %d-%d)",
                 svc->start_handle, svc->end_handle);
    }
    else if (error->status == BLE_HS_EDONE)
    {
        if (s_disc_start != 0)
        {
            uint16_t start = s_disc_start, end = s_disc_end;
            s_disc_start = 0;
            ble_gattc_disc_chrs_by_uuid(conn_handle, start, end,
                                        (ble_uuid_t *)&s_auth_chr_uuid,
                                        cli_disc_auth_chr_cb, NULL);
        }
        else
        {
            ESP_LOGW(TAG, "AUTH service not found");
            s_cycle_busy = false;
        }
    }
    return 0;
}

static void cli_start_open_discovery(void)
{
    s_disc_start = 0;
    s_disc_end = 0;
    ble_gattc_disc_svc_by_uuid(s_conn_handle,
                               (ble_uuid_t *)&s_open_svc_uuid,
                               cli_disc_open_svc_cb, NULL);
}

static void cli_start_auth_discovery(void)
{
    s_disc_start = 0;
    s_disc_end = 0;
    ble_gattc_disc_svc_by_uuid(s_conn_handle,
                               (ble_uuid_t *)&s_auth_svc_uuid,
                               cli_disc_auth_svc_cb, NULL);
}

static void cli_write_open(void)
{
    char payload[128];
    int len = vandal_payload_build(VANDAL_PROTO_BLE_OPEN, payload, sizeof(payload));
    ble_gattc_write_flat(s_conn_handle, s_open_val_handle,
                         payload, (uint16_t)len,
                         cli_write_cb,
                         (void *)(uintptr_t)VANDAL_PROTO_BLE_OPEN);
}

static void cli_write_auth(void)
{
    char payload[128];
    int len = vandal_payload_build(VANDAL_PROTO_BLE_AUTH, payload, sizeof(payload));
    ble_gattc_write_flat(s_conn_handle, s_auth_val_handle,
                         payload, (uint16_t)len,
                         cli_write_cb,
                         (void *)(uintptr_t)VANDAL_PROTO_BLE_AUTH);
}

static int cli_mtu_cb(uint16_t conn_handle,
                      const struct ble_gatt_error *error,
                      uint16_t mtu, void *arg)
{
    if (error->status == 0)
    {
        ESP_LOGI(TAG, "MTU negotiated: %d", (int)mtu);
    }
    else
    {
        ESP_LOGW(TAG, "MTU exchange failed (status=%d), using default",
                 error->status);
    }
    cli_start_open_discovery();
    return 0;
}

/* ── Slave GAP event handler ─────────────────────────────────────────────── */

static int slave_ble_gap_event_cb(struct ble_gap_event *event, void *arg);

static void ble_start_scan(void)
{
    struct ble_gap_disc_params disc_params = {
        .itvl = 0,
        .window = 0,
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 1,
    };
    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER,
                 &disc_params, slave_ble_gap_event_cb, NULL);
}

static int slave_ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type)
    {
    case BLE_GAP_EVENT_DISC:
    {
        struct ble_hs_adv_fields fields;
        ble_hs_adv_parse_fields(&fields, event->disc.data,
                                event->disc.length_data);
        if (fields.name != NULL && fields.name_len > 0)
        {
            if (strncmp((const char *)fields.name,
                        CONFIG_VANDAL_BLE_DEVICE_NAME,
                        fields.name_len) == 0)
            {
                ESP_LOGI(TAG, "Found server \"%s\", connecting...",
                         CONFIG_VANDAL_BLE_DEVICE_NAME);
                ble_gap_disc_cancel();
                ble_gap_connect(BLE_OWN_ADDR_PUBLIC,
                                &event->disc.addr,
                                30000, NULL,
                                slave_ble_gap_event_cb, NULL);
            }
        }
        break;
    }

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0)
        {
            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            ESP_LOGI(TAG, "Connected to server (handle=%d)", s_conn_handle);
            s_cycle_busy = true;
            ble_gattc_exchange_mtu(s_conn_handle, cli_mtu_cb, NULL);
        }
        else
        {
            ESP_LOGW(TAG, "Connection failed (status=%d)", event->connect.status);
            ble_start_scan();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        /* Erase stored bond to force fresh passkey pairing on reconnect.
         * Without this, NimBLE reuses the cached LTK for fast re-encryption
         * which may not be MITM-authenticated — causing AUTH reads to fail
         * with status 261 again and triggering a security-initiate loop. */
        ble_gap_unpair(&event->disconnect.conn.peer_id_addr);
        s_connected = false;
        s_open_val_handle = 0;
        s_auth_val_handle = 0;
        s_cycle_busy = false;
        ESP_LOGI(TAG, "Disconnected (reason=%d)", event->disconnect.reason);
        ble_start_scan();
        break;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_INPUT)
        {
            struct ble_sm_io pk = {
                .action = BLE_SM_IOACT_INPUT,
                .passkey = CONFIG_VANDAL_BLE_PIN,
            };
            ble_sm_inject_io(event->passkey.conn_handle, &pk);
            ESP_LOGI(TAG, "Injecting passkey: %06d", (int)CONFIG_VANDAL_BLE_PIN);
        }
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0)
        {
            ESP_LOGI(TAG, "Encryption established, retrying AUTH write...");
            if (s_auth_val_handle != 0)
            {
                cli_write_auth();
            }
            else
            {
                /* Handle was lost (e.g. timeout reset) — re-discover before writing */
                cli_start_auth_discovery();
            }
        }
        else
        {
            ESP_LOGW(TAG, "Encryption failed (status=%d)",
                     event->enc_change.status);
            s_cycle_busy = false;
        }
        break;

    default:
        break;
    }
    return 0;
}

/* ── Slave NimBLE host sync ──────────────────────────────────────────────── */

static void slave_ble_on_sync(void)
{
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;

    ble_start_scan();
    ESP_LOGI(TAG, "Client scanning for \"%s\"...", CONFIG_VANDAL_BLE_DEVICE_NAME);
}

/* ── Slave periodic re-read task ─────────────────────────────────────────── */

static void ble_slave_task(void *arg)
{
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_VANDAL_PAYLOAD_SEND_INTERVAL_MS));
        if (!s_connected)
            continue;

        if (s_cycle_busy)
        {
            TickType_t elapsed = xTaskGetTickCount() - s_cycle_start_tick;
            if (elapsed > pdMS_TO_TICKS(BLE_CYCLE_TIMEOUT_MS))
            {
                ESP_LOGW(TAG, "BLE cycle timeout, forcing reset");
                s_cycle_busy = false;
            }
            else
            {
                continue;
            }
        }

        s_cycle_busy = true;
        s_cycle_start_tick = xTaskGetTickCount();

        if (s_open_val_handle != 0)
        {
            cli_write_open();
        }
        else
        {
            cli_start_open_discovery();
        }
    }
}

/* =========================================================================
 * Common
 * ========================================================================= */

static void nimble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset (reason=%d)", reason);
}

static bool s_nimble_init = false;
static bool s_nimble_starting = false;

/* =========================================================================
 * Public API
 * ========================================================================= */

/* Master: one-shot init task */
static void ble_launch_task(void *arg)
{
    uuid_from_str(CONFIG_VANDAL_BLE_OPEN_SERVICE_UUID, &s_open_svc_uuid);
    derive_chr_uuid(&s_open_svc_uuid, &s_open_chr_uuid);
    uuid_from_str(CONFIG_VANDAL_BLE_AUTH_SERVICE_UUID, &s_auth_svc_uuid);
    derive_chr_uuid(&s_auth_svc_uuid, &s_auth_chr_uuid);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_att_set_preferred_mtu(256);

    ble_hs_cfg.sync_cb = master_ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_svc_gap_device_name_set(CONFIG_VANDAL_BLE_DEVICE_NAME);
    ble_gatts_count_cfg(s_gatt_svcs);
    ble_gatts_add_svcs(s_gatt_svcs);

    nimble_port_freertos_init(nimble_host_task);

    s_nimble_init = true;
    ESP_LOGI(TAG, "GATT server initialized (PIN=%06d)", CONFIG_VANDAL_BLE_PIN);
    vTaskDelete(NULL);
}

void vandal_ble_init(void)
{
    /* Init UUIDs for both roles */
    uuid_from_str(CONFIG_VANDAL_BLE_OPEN_SERVICE_UUID, &s_open_svc_uuid);
    derive_chr_uuid(&s_open_svc_uuid, &s_open_chr_uuid);
    uuid_from_str(CONFIG_VANDAL_BLE_AUTH_SERVICE_UUID, &s_auth_svc_uuid);
    derive_chr_uuid(&s_auth_svc_uuid, &s_auth_chr_uuid);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_att_set_preferred_mtu(256);

    if (vandal_is_master())
    {
        /* Master: start advertising the AUTH service immediately for demo
         * purposes. This exposes the vulnerable BLE protocol by default when
         * the board boots, allowing Vandal to probe and classify the security
         * profile as legacy_pairing (DISP_ONLY + MITM + no SC). */
        ble_hs_cfg.sync_cb = master_ble_on_sync;
        ble_hs_cfg.reset_cb = ble_on_reset;
        s_nimble_init = true;
    }
    else
    {
        /* Slave: init as GATT client (central) */
        ble_hs_cfg.sync_cb = slave_ble_on_sync;
        ble_hs_cfg.reset_cb = ble_on_reset;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();

    nimble_port_freertos_init(nimble_host_task);

    if (vandal_is_master())
    {
        /* Master: register GATT services (will advertise once on_sync fires) */
        ble_svc_gap_device_name_set(CONFIG_VANDAL_BLE_DEVICE_NAME);
        ble_gatts_count_cfg(s_gatt_svcs);
        ble_gatts_add_svcs(s_gatt_svcs);
        vandal_proto_set_running(VANDAL_PROTO_BLE_OPEN, true);
        vandal_proto_set_running(VANDAL_PROTO_BLE_AUTH, true);
        ESP_LOGI(TAG, "GATT server initialized; advertising open+auth services (PIN=%06d)",
                 CONFIG_VANDAL_BLE_PIN);
    }
    else
    {
        /* Slave: start client task */
        xTaskCreate(ble_slave_task, "ble_slave", 4096, NULL, 4, NULL);
        ESP_LOGI(TAG, "GATT client initialized (PIN=%06d)", CONFIG_VANDAL_BLE_PIN);
    }
}

void vandal_ble_start(void)
{
    if (!vandal_is_master()) return; /* no-op on slave */

    if (!s_nimble_init && !s_nimble_starting)
    {
        s_nimble_starting = true;
        xTaskCreate(ble_launch_task, "ble_init", 6144, NULL, 5, NULL);
    }
}

void vandal_ble_stop(void)
{
    if (!vandal_is_master()) return; /* no-op on slave */

    vandal_proto_set_running(VANDAL_PROTO_BLE_OPEN, false);
    vandal_proto_set_running(VANDAL_PROTO_BLE_AUTH, false);
    ESP_LOGI(TAG, "Stopped");
}
