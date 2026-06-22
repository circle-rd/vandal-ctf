/*
 * Vandal Test - I2C module implementation.
 *
 * ESP-IDF v5.x new I2C driver is used:
 *   - Master: i2c_master_bus + i2c_master_dev
 *   - Slave:  i2c_slave driver
 *
 * Role determined at runtime via vandal_get_role().
 */

#include "vandal_i2c.h"
#include "vandal_common.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/i2c_slave.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "vandal_i2c";

/* ---------------------------------------------------------------------------
 * Master
 * --------------------------------------------------------------------------- */

static i2c_master_bus_handle_t s_bus_handle;
static i2c_master_dev_handle_t s_dev_handle;
static bool s_hw_init = false;
static TaskHandle_t s_task_handle = NULL;

static void i2c_master_task(void *arg)
{
    char payload[128];
    while (1)
    {
        if (vandal_proto_is_running(VANDAL_PROTO_I2C))
        {
            int len = vandal_payload_build(VANDAL_PROTO_I2C, payload, sizeof(payload));
            esp_err_t err = i2c_master_transmit(s_dev_handle,
                                                (const uint8_t *)payload, len,
                                                pdMS_TO_TICKS(1000));
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
 * Slave
 * --------------------------------------------------------------------------- */

static i2c_slave_dev_handle_t s_slave_handle;

#define I2C_SLAVE_RX_BUF 256
static uint8_t s_rx_buf[I2C_SLAVE_RX_BUF];
static volatile size_t s_rx_len = 0;

static IRAM_ATTR bool i2c_slave_rx_cb(i2c_slave_dev_handle_t channel,
                                      const i2c_slave_rx_done_event_data_t *edata,
                                      void *user_data)
{
    size_t len = edata->length;
    if (len > I2C_SLAVE_RX_BUF)
    {
        len = I2C_SLAVE_RX_BUF;
    }
    memcpy(s_rx_buf, edata->buffer, len);
    s_rx_len = len;
    return false;
}

static void i2c_slave_task(void *arg)
{
    while (1)
    {
        if (s_rx_len > 0)
        {
            size_t len = s_rx_len;
            s_rx_len = 0;

            vandal_event_payload_t evt = {
                .protocol = VANDAL_PROTO_I2C,
                .payload_len = len,
            };
            if (len >= sizeof(evt.payload))
            {
                len = sizeof(evt.payload) - 1;
            }
            memcpy(evt.payload, s_rx_buf, len);
            evt.payload[len] = '\0';

            esp_event_post(VANDAL_EVENT, VANDAL_EVT_PAYLOAD_RECEIVED,
                           &evt, sizeof(evt), portMAX_DELAY);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

void vandal_i2c_init(void)
{
    if (vandal_is_master())
        return; /* master uses lazy init via start() */

    i2c_slave_config_t slave_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_VANDAL_I2C_SDA_PIN,
        .scl_io_num = CONFIG_VANDAL_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .send_buf_depth = 256,
        .receive_buf_depth = 256,
        .slave_addr = CONFIG_VANDAL_I2C_SLAVE_ADDR,
        .addr_bit_len = I2C_ADDR_BIT_LEN_7,
    };
    ESP_ERROR_CHECK(i2c_new_slave_device(&slave_cfg, &s_slave_handle));

    i2c_slave_event_callbacks_t cbs = {
        .on_receive = i2c_slave_rx_cb,
    };
    ESP_ERROR_CHECK(i2c_slave_register_event_callbacks(s_slave_handle, &cbs, NULL));

    ESP_LOGI(TAG, "Slave initialized (SDA=%d, SCL=%d, addr=0x%02x)",
             CONFIG_VANDAL_I2C_SDA_PIN, CONFIG_VANDAL_I2C_SCL_PIN,
             CONFIG_VANDAL_I2C_SLAVE_ADDR);

    xTaskCreate(i2c_slave_task, "i2c_slave", 4096, NULL, 5, NULL);
}

void vandal_i2c_start(void)
{
    if (!vandal_is_master())
        return; /* no-op on slave */

    if (!s_hw_init)
    {
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = CONFIG_VANDAL_I2C_SDA_PIN,
            .scl_io_num = CONFIG_VANDAL_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus_handle));

        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = CONFIG_VANDAL_I2C_SLAVE_ADDR,
            .scl_speed_hz = CONFIG_VANDAL_I2C_FREQ_HZ,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &s_dev_handle));

        ESP_LOGI(TAG, "Master HW initialized (SDA=%d, SCL=%d, addr=0x%02x)",
                 CONFIG_VANDAL_I2C_SDA_PIN, CONFIG_VANDAL_I2C_SCL_PIN,
                 CONFIG_VANDAL_I2C_SLAVE_ADDR);
        s_hw_init = true;
        xTaskCreate(i2c_master_task, "i2c_master", 4096, NULL, 5, &s_task_handle);
    }
    vandal_proto_set_running(VANDAL_PROTO_I2C, true);
    ESP_LOGI(TAG, "Started");
}

void vandal_i2c_stop(void)
{
    if (!vandal_is_master())
        return; /* no-op on slave */

    vandal_proto_set_running(VANDAL_PROTO_I2C, false);
    ESP_LOGI(TAG, "Stopped");
}
