/*
 * Vandal Test - SPI module implementation.
 *
 * Master uses spi_master driver, Slave uses spi_slave driver.
 * Both operate on SPI2_HOST (the general-purpose SPI on ESP32-C6).
 *
 * Role determined at runtime via vandal_get_role().
 */

#include "vandal_spi.h"
#include "vandal_common.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/spi_slave.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "vandal_spi";

#define SPI_PAYLOAD_MAX 128

/* ---------------------------------------------------------------------------
 * Master
 * --------------------------------------------------------------------------- */

static spi_device_handle_t s_spi_dev;
static bool s_hw_init = false;
static TaskHandle_t s_task_handle = NULL;

static void spi_master_task(void *arg)
{
    char payload[SPI_PAYLOAD_MAX];
    while (1)
    {
        if (vandal_proto_is_running(VANDAL_PROTO_SPI))
        {
            int len = vandal_payload_build(VANDAL_PROTO_SPI, payload, sizeof(payload));

            spi_transaction_t txn = {
                .length = len * 8,
                .tx_buffer = payload,
            };

            esp_err_t err = spi_device_transmit(s_spi_dev, &txn);
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

static void spi_slave_task(void *arg)
{
    WORD_ALIGNED_ATTR uint8_t rx_buf[SPI_PAYLOAD_MAX];

    while (1)
    {
        memset(rx_buf, 0, sizeof(rx_buf));

        spi_slave_transaction_t txn = {
            .length = SPI_PAYLOAD_MAX * 8,
            .rx_buffer = rx_buf,
        };

        esp_err_t err = spi_slave_transmit(SPI2_HOST, &txn, portMAX_DELAY);
        if (err == ESP_OK && txn.trans_len > 0)
        {
            size_t byte_len = txn.trans_len / 8;
            if (byte_len > sizeof(rx_buf) - 1)
            {
                byte_len = sizeof(rx_buf) - 1;
            }
            rx_buf[byte_len] = '\0';

            vandal_event_payload_t evt = {
                .protocol = VANDAL_PROTO_SPI,
                .payload_len = byte_len,
            };
            memcpy(evt.payload, rx_buf, byte_len + 1);

            esp_event_post(VANDAL_EVENT, VANDAL_EVT_PAYLOAD_RECEIVED,
                           &evt, sizeof(evt), portMAX_DELAY);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

void vandal_spi_init(void)
{
    if (vandal_is_master())
        return; /* master uses lazy init via start() */

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_VANDAL_SPI_SLAVE_MOSI_PIN,
        .miso_io_num = CONFIG_VANDAL_SPI_SLAVE_MISO_PIN,
        .sclk_io_num = CONFIG_VANDAL_SPI_SLAVE_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_PAYLOAD_MAX,
    };

    spi_slave_interface_config_t slave_cfg = {
        .mode = 0,
        .spics_io_num = CONFIG_VANDAL_SPI_SLAVE_CS_PIN,
        .queue_size = 4,
        .flags = 0,
    };
    ESP_ERROR_CHECK(spi_slave_initialize(SPI2_HOST, &bus_cfg, &slave_cfg,
                                         SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Slave initialized (MOSI=%d, MISO=%d, SCLK=%d, CS=%d)",
             CONFIG_VANDAL_SPI_SLAVE_MOSI_PIN, CONFIG_VANDAL_SPI_SLAVE_MISO_PIN,
             CONFIG_VANDAL_SPI_SLAVE_SCLK_PIN, CONFIG_VANDAL_SPI_SLAVE_CS_PIN);

    xTaskCreate(spi_slave_task, "spi_slave", 4096, NULL, 5, NULL);
}

void vandal_spi_start(void)
{
    if (!vandal_is_master())
        return; /* no-op on slave */

    if (!s_hw_init)
    {
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = CONFIG_VANDAL_SPI_MASTER_MOSI_PIN,
            .miso_io_num = CONFIG_VANDAL_SPI_MASTER_MISO_PIN,
            .sclk_io_num = CONFIG_VANDAL_SPI_MASTER_SCLK_PIN,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = SPI_PAYLOAD_MAX,
        };
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

        spi_device_interface_config_t dev_cfg = {
            .clock_speed_hz = CONFIG_VANDAL_SPI_CLOCK_SPEED_HZ,
            .mode = 0,
            .spics_io_num = CONFIG_VANDAL_SPI_MASTER_CS_PIN,
            .queue_size = 4,
        };
        ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi_dev));

        ESP_LOGI(TAG, "Master HW initialized (MOSI=%d, MISO=%d, SCLK=%d, CS=%d)",
                 CONFIG_VANDAL_SPI_MASTER_MOSI_PIN, CONFIG_VANDAL_SPI_MASTER_MISO_PIN,
                 CONFIG_VANDAL_SPI_MASTER_SCLK_PIN, CONFIG_VANDAL_SPI_MASTER_CS_PIN);
        s_hw_init = true;
        xTaskCreate(spi_master_task, "spi_master", 4096, NULL, 5, &s_task_handle);
    }
    vandal_proto_set_running(VANDAL_PROTO_SPI, true);
    ESP_LOGI(TAG, "Started");
}

void vandal_spi_stop(void)
{
    if (!vandal_is_master())
        return; /* no-op on slave */

    vandal_proto_set_running(VANDAL_PROTO_SPI, false);
    ESP_LOGI(TAG, "Stopped");
}
