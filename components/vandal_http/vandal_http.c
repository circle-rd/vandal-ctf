/*
 * Vandal Test - HTTP module implementation.
 *
 * Master: lightweight HTTP server on the open SoftAP.
 *   - POST /messages   : receive slave JSON report
 *   - GET  /status     : return last slave report
 *   - GET  /system     : system info + protocol running states
 *   - POST /control    : start / stop a protocol
 *   - POST /payload    : set custom payload for a protocol
 *   - GET  / *          : serve static files from FATFS
 *
 * Slave:  periodic JSON POST of protocol status to master.
 */

#include "vandal_http.h"
#include "vandal_common.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "sdkconfig.h"
#include "cJSON.h"

#include "esp_http_server.h"
#include "esp_http_client.h"
#include <sys/stat.h>
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "driver/temperature_sensor.h"

/* Protocol modules (for start/stop dispatch) */
#include "vandal_uart.h"
#include "vandal_i2c.h"
#include "vandal_spi.h"
#include "vandal_espnow.h"
#include "vandal_ble.h"
#include "vandal_thread.h"

static const char *TAG = "vandal_http";

/* =========================================================================
 * MASTER — HTTP Server
 * ========================================================================= */

#define MOUNT_POINT "/website"
#define FILE_CHUNK_SZ 1024

/* Last JSON report received from the slave (returned verbatim by /status) */
static char s_last_report[1024] = "{}";

/* Master-originated payloads (Thread TX, HTTP TX) self-reported via event bus.
 * These are merged into /status so the frontend reflects master TX activity
 * even before the slave echoes the data back through /messages. */
static char s_master_payloads[VANDAL_PROTO_MAX][128];
static bool s_master_received[VANDAL_PROTO_MAX];

static void master_payload_event_handler(void *handler_arg,
                                         esp_event_base_t base,
                                         int32_t id,
                                         void *event_data)
{
    if (base != VANDAL_EVENT || id != VANDAL_EVT_PAYLOAD_RECEIVED)
        return;
    const vandal_event_payload_t *evt = (const vandal_event_payload_t *)event_data;
    if (evt->protocol < VANDAL_PROTO_MAX)
    {
        strncpy(s_master_payloads[evt->protocol], evt->payload,
                sizeof(s_master_payloads[0]) - 1);
        s_master_payloads[evt->protocol][sizeof(s_master_payloads[0]) - 1] = '\0';
        s_master_received[evt->protocol] = true;
    }
}

/* Temperature sensor handle (initialized once) */
static temperature_sensor_handle_t s_temp_sensor = NULL;

static void ensure_temp_sensor(void)
{
    if (s_temp_sensor)
        return;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
    if (temperature_sensor_install(&cfg, &s_temp_sensor) == ESP_OK)
    {
        temperature_sensor_enable(s_temp_sensor);
    }
}

/* ── Content-type mapping ────────────────────────────────────────────────── */

static const char *get_content_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot)
        return "application/octet-stream";
    if (strcmp(dot, ".html") == 0)
        return "text/html";
    if (strcmp(dot, ".js") == 0)
        return "application/javascript";
    if (strcmp(dot, ".css") == 0)
        return "text/css";
    if (strcmp(dot, ".json") == 0)
        return "application/json";
    if (strcmp(dot, ".png") == 0)
        return "image/png";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(dot, ".svg") == 0)
        return "image/svg+xml";
    if (strcmp(dot, ".ico") == 0)
        return "image/x-icon";
    if (strcmp(dot, ".woff") == 0)
        return "font/woff";
    if (strcmp(dot, ".woff2") == 0)
        return "font/woff2";
    return "application/octet-stream";
}

/* ── POST /messages handler ──────────────────────────────────────────────── */

static esp_err_t messages_post_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len > (int)sizeof(s_last_report) - 1)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_FAIL;
    }

    char *buf = malloc(total_len + 1);
    if (!buf)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total_len)
    {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0)
        {
            free(buf);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT)
                httpd_resp_send_408(req);
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[total_len] = '\0';

    /* Store verbatim for /status */
    strncpy(s_last_report, buf, sizeof(s_last_report) - 1);
    s_last_report[sizeof(s_last_report) - 1] = '\0';

    /* Parse and log */
    cJSON *root = cJSON_Parse(buf);
    if (root)
    {
        ESP_LOGI(TAG, "── Slave report ──────────────────────────");
        for (int p = 0; p < VANDAL_PROTO_MAX; p++)
        {
            const char *key = vandal_proto_name((vandal_proto_t)p);
            cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
            if (cJSON_IsString(item) && item->valuestring)
                ESP_LOGI(TAG, "  [%s] %s", key, item->valuestring);
            else
                ESP_LOGW(TAG, "  [%s] (null / not received)", key);
        }
        ESP_LOGI(TAG, "──────────────────────────────────────────");
        cJSON_Delete(root);
    }
    else
    {
        ESP_LOGW(TAG, "Invalid JSON from slave: %s", buf);
    }

    free(buf);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* ── GET /status handler ─────────────────────────────────────────────────── */

static esp_err_t status_get_handler(httpd_req_t *req)
{
    /* Merge slave report with any master-originated payloads.
     * Master TX events (Thread, HTTP) are self-reported immediately;
     * slave data arrives later via POST /messages. The merged view shows
     * the most recent value regardless of which side produced it. */
    cJSON *root = cJSON_Parse(s_last_report);
    if (!root)
    {
        root = cJSON_CreateObject();
    }
    if (root)
    {
        for (int p = 0; p < VANDAL_PROTO_MAX; p++)
        {
            if (!s_master_received[p])
                continue;
            const char *key = vandal_proto_name((vandal_proto_t)p);
            /* Only override if slave has no data for this key yet */
            cJSON *existing = cJSON_GetObjectItemCaseSensitive(root, key);
            if (!cJSON_IsString(existing) || existing->valuestring == NULL
                || existing->valuestring[0] == '\0')
            {
                cJSON_DeleteItemFromObjectCaseSensitive(root, key);
                cJSON_AddStringToObject(root, key, s_master_payloads[p]);
            }
        }
        char *json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_sendstr(req, json ? json : s_last_report);
        free(json);
    }
    else
    {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_sendstr(req, s_last_report);
    }
    return ESP_OK;
}

/* ── GET /system handler — system info + protocol states ─────────────────── */

static esp_err_t system_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    /* Heap info */
    size_t heap_total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    size_t heap_free = esp_get_free_heap_size();
    size_t heap_min = esp_get_minimum_free_heap_size();
    cJSON_AddNumberToObject(root, "heap_total", (double)heap_total);
    cJSON_AddNumberToObject(root, "heap_free", (double)heap_free);
    cJSON_AddNumberToObject(root, "heap_min_free", (double)heap_min);

    /* Temperature */
    ensure_temp_sensor();
    float temp_c = 0;
    if (s_temp_sensor)
    {
        temperature_sensor_get_celsius(s_temp_sensor, &temp_c);
    }
    cJSON_AddNumberToObject(root, "temperature", temp_c);

    /* Uptime */
    int64_t uptime_us = esp_timer_get_time();
    cJSON_AddNumberToObject(root, "uptime_s", (double)(uptime_us / 1000000));

    /* Task count */
    cJSON_AddNumberToObject(root, "task_count", (double)uxTaskGetNumberOfTasks());

    /* IDF version */
    cJSON_AddStringToObject(root, "idf_version", esp_get_idf_version());

    /* Protocol running states + custom payloads */
    cJSON *protos = cJSON_AddObjectToObject(root, "protocols");
    for (int i = 0; i < VANDAL_PROTO_MAX; i++)
    {
        cJSON *p = cJSON_AddObjectToObject(protos, vandal_proto_name((vandal_proto_t)i));
        cJSON_AddBoolToObject(p, "running", vandal_proto_is_running((vandal_proto_t)i));
        const char *custom = vandal_proto_get_custom_payload((vandal_proto_t)i);
        if (custom)
        {
            cJSON_AddStringToObject(p, "custom_payload", custom);
        }
        else
        {
            cJSON_AddNullToObject(p, "custom_payload");
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

/* ── HTTP Payload service ─────────────────────────────────────────────────
 * Master: POST /http-payload endpoint receives payload from slave, fires event.
 * Slave:  when service started, a task periodically POSTs custom payload.
 * ─────────────────────────────────────────────────────────────────────────── */

static char *read_post_body(httpd_req_t *req);

/* Slave sender task handle (NULL = not running) */
static TaskHandle_t s_http_payload_task = NULL;

static void http_payload_sender_task(void *arg)
{
    /* Wait for WiFi connection */
    vTaskDelay(pdMS_TO_TICKS(2000));

    char url[96];
    snprintf(url, sizeof(url), "http://" CONFIG_VANDAL_HTTP_MASTER_IP ":%d/http-payload",
             CONFIG_VANDAL_HTTP_PORT);

    while (1)
    {
        char payload[128];
        int len = vandal_payload_build(VANDAL_PROTO_HTTP, payload, sizeof(payload));

        /* Fire event locally so http_event_handler can include it in /messages */
        vandal_event_payload_t self_evt = {
            .protocol = VANDAL_PROTO_HTTP,
            .payload_len = (size_t)len,
        };
        strncpy(self_evt.payload, payload, sizeof(self_evt.payload) - 1);
        self_evt.payload[sizeof(self_evt.payload) - 1] = '\0';
        esp_event_post(VANDAL_EVENT, VANDAL_EVT_PAYLOAD_RECEIVED,
                       &self_evt, sizeof(self_evt), pdMS_TO_TICKS(100));

        esp_http_client_config_t cfg = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 3000,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client)
        {
            esp_http_client_set_header(client, "Content-Type", "text/plain");
            esp_http_client_set_post_field(client, payload, len);

            esp_err_t err = esp_http_client_perform(client);
            if (err == ESP_OK)
            {
                ESP_LOGI(TAG, "[HTTP] POST /http-payload → %d",
                         esp_http_client_get_status_code(client));
            }
            else
            {
                ESP_LOGW(TAG, "[HTTP] POST failed: %s", esp_err_to_name(err));
            }
            esp_http_client_cleanup(client);
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_VANDAL_HTTP_POST_INTERVAL_MS));
    }
}

static void http_payload_start(void)
{
    if (s_http_payload_task != NULL)
        return; /* already running */

    vandal_proto_set_running(VANDAL_PROTO_HTTP, true);

    if (!vandal_is_master())
    {
        xTaskCreate(http_payload_sender_task, "http_payload", 4096, NULL, 3,
                    &s_http_payload_task);
    }
    ESP_LOGI(TAG, "HTTP payload service started");
}

static void http_payload_stop(void)
{
    vandal_proto_set_running(VANDAL_PROTO_HTTP, false);

    if (s_http_payload_task != NULL)
    {
        vTaskDelete(s_http_payload_task);
        s_http_payload_task = NULL;
    }
    ESP_LOGI(TAG, "HTTP payload service stopped");
}

/* Master handler: POST /http-payload */
static esp_err_t http_payload_recv_handler(httpd_req_t *req)
{
    char *body = read_post_body(req);
    if (!body)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[HTTP] RX payload: %s", body);

    /* Fire event for central logging + status reporting */
    vandal_event_payload_t evt = {
        .protocol = VANDAL_PROTO_HTTP,
    };
    strncpy(evt.payload, body, sizeof(evt.payload) - 1);
    evt.payload[sizeof(evt.payload) - 1] = '\0';
    evt.payload_len = strlen(evt.payload);
    esp_event_post(VANDAL_EVENT, VANDAL_EVT_PAYLOAD_RECEIVED, &evt, sizeof(evt),
                   pdMS_TO_TICKS(100));

    free(body);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

/* ── Protocol start/stop dispatch ────────────────────────────────────────── */

static void dispatch_start(vandal_proto_t proto)
{
    switch (proto)
    {
    case VANDAL_PROTO_UART:
        vandal_uart_start();
        break;
    case VANDAL_PROTO_I2C:
        vandal_i2c_start();
        break;
    case VANDAL_PROTO_SPI:
        vandal_spi_start();
        break;
    case VANDAL_PROTO_ESPNOW:
        vandal_espnow_start();
        break;
    case VANDAL_PROTO_BLE_OPEN:
        vandal_ble_start();
        vandal_proto_set_running(VANDAL_PROTO_BLE_OPEN, true);
        break;
    case VANDAL_PROTO_BLE_AUTH:
        vandal_ble_start();
        vandal_proto_set_running(VANDAL_PROTO_BLE_AUTH, true);
        break;
    case VANDAL_PROTO_THREAD:
        vandal_thread_start();
        break;
    case VANDAL_PROTO_HTTP:
        http_payload_start();
        break;
    default:
        break;
    }
}

static void dispatch_stop(vandal_proto_t proto)
{
    switch (proto)
    {
    case VANDAL_PROTO_UART:
        vandal_uart_stop();
        break;
    case VANDAL_PROTO_I2C:
        vandal_i2c_stop();
        break;
    case VANDAL_PROTO_SPI:
        vandal_spi_stop();
        break;
    case VANDAL_PROTO_ESPNOW:
        vandal_espnow_stop();
        break;
    case VANDAL_PROTO_BLE_OPEN:
        vandal_proto_set_running(VANDAL_PROTO_BLE_OPEN, false);
        break;
    case VANDAL_PROTO_BLE_AUTH:
        vandal_proto_set_running(VANDAL_PROTO_BLE_AUTH, false);
        break;
    case VANDAL_PROTO_THREAD:
        vandal_thread_stop();
        break;
    case VANDAL_PROTO_HTTP:
        http_payload_stop();
        break;
    default:
        break;
    }
}

/* ── Helper: read full POST body into malloc'd buffer ────────────────────── */

static char *read_post_body(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 1024)
        return NULL;

    char *buf = malloc(total + 1);
    if (!buf)
        return NULL;

    int off = 0;
    while (off < total)
    {
        int r = httpd_req_recv(req, buf + off, total - off);
        if (r <= 0)
        {
            free(buf);
            return NULL;
        }
        off += r;
    }
    buf[total] = '\0';
    return buf;
}

/* ── POST /control handler ───────────────────────────────────────────────── */

static esp_err_t control_post_handler(httpd_req_t *req)
{
    char *body = read_post_body(req);
    if (!body)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *j_proto = cJSON_GetObjectItemCaseSensitive(root, "protocol");
    cJSON *j_action = cJSON_GetObjectItemCaseSensitive(root, "action");

    if (!cJSON_IsString(j_proto) || !cJSON_IsString(j_action))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Need protocol + action");
        return ESP_FAIL;
    }

    vandal_proto_t proto = vandal_proto_from_name(j_proto->valuestring);
    if (proto >= VANDAL_PROTO_MAX)
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown protocol");
        return ESP_FAIL;
    }

    if (strcmp(j_action->valuestring, "start") == 0)
    {
        dispatch_start(proto);
        ESP_LOGI(TAG, "Protocol %s → START", j_proto->valuestring);
    }
    else if (strcmp(j_action->valuestring, "stop") == 0)
    {
        dispatch_stop(proto);
        ESP_LOGI(TAG, "Protocol %s → STOP", j_proto->valuestring);
    }
    else
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "action must be start|stop");
        return ESP_FAIL;
    }

    cJSON_Delete(root);

    /* Respond with current state */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddStringToObject(resp, "protocol", vandal_proto_name(proto));
    cJSON_AddBoolToObject(resp, "running", vandal_proto_is_running(proto));
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

/* ── POST /payload handler ───────────────────────────────────────────────── */

static esp_err_t payload_post_handler(httpd_req_t *req)
{
    char *body = read_post_body(req);
    if (!body)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *j_proto = cJSON_GetObjectItemCaseSensitive(root, "protocol");
    cJSON *j_payload = cJSON_GetObjectItemCaseSensitive(root, "payload");

    if (!cJSON_IsString(j_proto))
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Need protocol");
        return ESP_FAIL;
    }

    vandal_proto_t proto = vandal_proto_from_name(j_proto->valuestring);
    if (proto >= VANDAL_PROTO_MAX)
    {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown protocol");
        return ESP_FAIL;
    }

    if (cJSON_IsString(j_payload) && strlen(j_payload->valuestring) > 0)
    {
        vandal_proto_set_custom_payload(proto, j_payload->valuestring);
        ESP_LOGI(TAG, "Custom payload for %s: \"%s\"",
                 vandal_proto_name(proto), j_payload->valuestring);
    }
    else
    {
        vandal_proto_set_custom_payload(proto, NULL);
        ESP_LOGI(TAG, "Custom payload for %s cleared", vandal_proto_name(proto));
    }

    cJSON_Delete(root);

    /* Respond */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddStringToObject(resp, "protocol", vandal_proto_name(proto));
    const char *cp = vandal_proto_get_custom_payload(proto);
    if (cp)
        cJSON_AddStringToObject(resp, "payload", cp);
    else
        cJSON_AddNullToObject(resp, "payload");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

/* ── OPTIONS handler for CORS preflight ──────────────────────────────────── */

static esp_err_t cors_options_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ── GET / * handler — serve static files from FATFS ───────────────────────── */

static esp_err_t static_file_handler(httpd_req_t *req)
{
    char filepath[256];
    const char *uri = req->uri;

    /* Strip query string if present */
    const char *q = strchr(uri, '?');
    int uri_len = q ? (int)(q - uri) : (int)strlen(uri);

    if (uri_len <= 1)
    {
        snprintf(filepath, sizeof(filepath), MOUNT_POINT "/index.html");
    }
    else
    {
        snprintf(filepath, sizeof(filepath), MOUNT_POINT "%.*s", uri_len, uri);
    }

    /* Try to open the file */
    struct stat st;
    if (stat(filepath, &st) != 0)
    {
        /* SPA fallback: serve index.html for any unknown path */
        snprintf(filepath, sizeof(filepath), MOUNT_POINT "/index.html");
        if (stat(filepath, &st) != 0)
        {
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }
    }

    FILE *f = fopen(filepath, "rb");
    if (!f)
    {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, get_content_type(filepath));

    /* Send in chunks */
    char *chunk = malloc(FILE_CHUNK_SZ);
    if (!chunk)
    {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }

    size_t n;
    while ((n = fread(chunk, 1, FILE_CHUNK_SZ, f)) > 0)
    {
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK)
        {
            fclose(f);
            free(chunk);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }

    fclose(f);
    free(chunk);
    httpd_resp_send_chunk(req, NULL, 0); /* end chunked response */
    return ESP_OK;
}

/* ── Server startup ──────────────────────────────────────────────────────── */

static httpd_handle_t s_server = NULL;

static void start_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_VANDAL_HTTP_PORT;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    config.max_uri_handlers = 16;

    if (httpd_start(&s_server, &config) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    /* Order matters: specific routes first, wildcard last */

    const httpd_uri_t msg_uri = {
        .uri = "/messages",
        .method = HTTP_POST,
        .handler = messages_post_handler,
    };
    httpd_register_uri_handler(s_server, &msg_uri);

    const httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    httpd_register_uri_handler(s_server, &status_uri);

    const httpd_uri_t system_uri = {
        .uri = "/system",
        .method = HTTP_GET,
        .handler = system_get_handler,
    };
    httpd_register_uri_handler(s_server, &system_uri);

    const httpd_uri_t control_uri = {
        .uri = "/control",
        .method = HTTP_POST,
        .handler = control_post_handler,
    };
    httpd_register_uri_handler(s_server, &control_uri);

    const httpd_uri_t payload_uri = {
        .uri = "/payload",
        .method = HTTP_POST,
        .handler = payload_post_handler,
    };
    httpd_register_uri_handler(s_server, &payload_uri);

    const httpd_uri_t http_payload_uri = {
        .uri = "/http-payload",
        .method = HTTP_POST,
        .handler = http_payload_recv_handler,
    };
    httpd_register_uri_handler(s_server, &http_payload_uri);

    /* CORS preflight for /control, /payload, and /http-payload */
    const httpd_uri_t cors_control = {
        .uri = "/control",
        .method = HTTP_OPTIONS,
        .handler = cors_options_handler,
    };
    httpd_register_uri_handler(s_server, &cors_control);

    const httpd_uri_t cors_payload = {
        .uri = "/payload",
        .method = HTTP_OPTIONS,
        .handler = cors_options_handler,
    };
    httpd_register_uri_handler(s_server, &cors_payload);

    const httpd_uri_t cors_http_payload = {
        .uri = "/http-payload",
        .method = HTTP_OPTIONS,
        .handler = cors_options_handler,
    };
    httpd_register_uri_handler(s_server, &cors_http_payload);

    const httpd_uri_t static_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = static_file_handler,
    };
    httpd_register_uri_handler(s_server, &static_uri);

    /* Register master self-report handler to capture Thread/HTTP TX events */
    ESP_ERROR_CHECK(esp_event_handler_register(
        VANDAL_EVENT, VANDAL_EVT_PAYLOAD_RECEIVED,
        master_payload_event_handler, NULL));

    ESP_LOGI(TAG, "HTTP server started on port %d (static root: " MOUNT_POINT ")",
             CONFIG_VANDAL_HTTP_PORT);
}

static void start_client(void);

/* =========================================================================
 * SLAVE — HTTP Client (periodic JSON POST)
 * ========================================================================= */

/* Latest payloads per protocol (protected by a simple flag, no mutex needed
 * since we only write from the event handler and read from the POST task). */
static char s_payloads[VANDAL_PROTO_MAX][128];
static bool s_received[VANDAL_PROTO_MAX];

/* ── Event handler — stores latest payload per protocol ──────────────────── */

static void http_event_handler(void *handler_arg,
                               esp_event_base_t base,
                               int32_t id,
                               void *event_data)
{
    if (base != VANDAL_EVENT || id != VANDAL_EVT_PAYLOAD_RECEIVED)
        return;

    const vandal_event_payload_t *evt = (const vandal_event_payload_t *)event_data;
    if (evt->protocol < VANDAL_PROTO_MAX)
    {
        strncpy(s_payloads[evt->protocol], evt->payload,
                sizeof(s_payloads[0]) - 1);
        s_payloads[evt->protocol][sizeof(s_payloads[0]) - 1] = '\0';
        s_received[evt->protocol] = true;
    }
}

/* ── Build JSON and POST to master ───────────────────────────────────────── */

static void post_report(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL)
        return;

    for (int p = 0; p < VANDAL_PROTO_MAX; p++)
    {
        const char *key = vandal_proto_name((vandal_proto_t)p);
        if (s_received[p])
        {
            cJSON_AddStringToObject(root, key, s_payloads[p]);
        }
        else
        {
            cJSON_AddNullToObject(root, key);
        }
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str == NULL)
        return;

    /* POST to master's AP IP */
    char url[64];
    snprintf(url, sizeof(url), "http://" CONFIG_VANDAL_HTTP_MASTER_IP ":%d/messages",
             CONFIG_VANDAL_HTTP_PORT);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 3000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        free(json_str);
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_str, strlen(json_str));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "POST /messages → %d (%d bytes)",
                 status, (int)strlen(json_str));
    }
    else
    {
        ESP_LOGW(TAG, "POST /messages failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(json_str);
}

/* ── Background POST task ────────────────────────────────────────────────── */

static void http_client_task(void *arg)
{
    /* Wait a bit for WiFi STA to connect and get an IP */
    vTaskDelay(pdMS_TO_TICKS(5000));

    while (1)
    {
        post_report();
        vTaskDelay(pdMS_TO_TICKS(CONFIG_VANDAL_HTTP_POST_INTERVAL_MS));
    }
}

static void start_client(void)
{
    /* Register our own event handler to collect payloads */
    ESP_ERROR_CHECK(esp_event_handler_register(
        VANDAL_EVENT, VANDAL_EVT_PAYLOAD_RECEIVED,
        http_event_handler, NULL));

    xTaskCreate(http_client_task, "http_client", 8192, NULL, 3, NULL);

    /* Start sending HTTP payloads to master's /http-payload endpoint */
    http_payload_start();

    ESP_LOGI(TAG, "HTTP client initialized (POST to %s:%d every %d ms)",
             CONFIG_VANDAL_HTTP_MASTER_IP, CONFIG_VANDAL_HTTP_PORT,
             CONFIG_VANDAL_HTTP_POST_INTERVAL_MS);
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void vandal_http_init(void)
{
    if (vandal_is_master())
    {
        start_server();
    }
    else
    {
        start_client();
    }
}
