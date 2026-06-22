/*
 * Vandal Test - UART module implementation.
 *
 * Role determined at runtime via vandal_get_role().
 */

#include "vandal_uart.h"
#include "vandal_common.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "vandal_uart";

#define UART_PORT_NUM UART_NUM_1
#define UART_BUF_SIZE 256

/* Pin selection at runtime */
static int uart_tx_pin(void)
{
    return vandal_is_master() ? CONFIG_VANDAL_UART_MASTER_TX_PIN
                              : CONFIG_VANDAL_UART_SLAVE_TX_PIN;
}

static int uart_rx_pin(void)
{
    return vandal_is_master() ? CONFIG_VANDAL_UART_MASTER_RX_PIN
                              : CONFIG_VANDAL_UART_SLAVE_RX_PIN;
}

/* ── Master task ─────────────────────────────────────────────────────────── */

static bool s_hw_init = false;
static TaskHandle_t s_task_handle = NULL;

static void uart_master_task(void *arg)
{
    char payload[128];
    while (1)
    {
        if (vandal_proto_is_running(VANDAL_PROTO_UART))
        {
            int len = vandal_payload_build(VANDAL_PROTO_UART, payload, sizeof(payload));
            uart_write_bytes(UART_PORT_NUM, payload, len);
            uart_write_bytes(UART_PORT_NUM, "\n", 1);
            ESP_LOGI(TAG, "TX: %s", payload);
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_VANDAL_PAYLOAD_SEND_INTERVAL_MS));
    }
}

/* ── Slave task ──────────────────────────────────────────────────────────── */

static void uart_slave_task(void *arg)
{
    uint8_t buf[UART_BUF_SIZE];
    while (1)
    {
        int len = uart_read_bytes(UART_PORT_NUM, buf, UART_BUF_SIZE - 1,
                                  pdMS_TO_TICKS(100));
        if (len > 0)
        {
            buf[len] = '\0';
            /* Strip trailing newline if present */
            if (len > 0 && buf[len - 1] == '\n')
            {
                buf[len - 1] = '\0';
                len--;
            }

            vandal_event_payload_t evt = {
                .protocol = VANDAL_PROTO_UART,
                .payload_len = len,
            };
            memcpy(evt.payload, buf, len + 1);

            esp_event_post(VANDAL_EVENT, VANDAL_EVT_PAYLOAD_RECEIVED,
                           &evt, sizeof(evt), portMAX_DELAY);
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void vandal_uart_init(void)
{
    if (vandal_is_master())
        return; /* master uses lazy init via start() */

    uart_config_t uart_config = {
        .baud_rate = CONFIG_VANDAL_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    int tx = uart_tx_pin();
    int rx = uart_rx_pin();

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE * 2,
                                        UART_BUF_SIZE * 2, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, tx, rx,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "Slave initialized (TX=%d, RX=%d, baud=%d)",
             tx, rx, CONFIG_VANDAL_UART_BAUD_RATE);

    xTaskCreate(uart_slave_task, "uart_slave", 4096, NULL, 5, NULL);
}

void vandal_uart_start(void)
{
    if (!vandal_is_master())
        return; /* no-op on slave */

    if (!s_hw_init)
    {
        uart_config_t uart_config = {
            .baud_rate = CONFIG_VANDAL_UART_BAUD_RATE,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };

        int tx = uart_tx_pin();
        int rx = uart_rx_pin();

        ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE * 2,
                                            UART_BUF_SIZE * 2, 0, NULL, 0));
        ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
        ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, tx, rx,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        ESP_LOGI(TAG, "Master HW initialized (TX=%d, RX=%d, baud=%d)",
                 tx, rx, CONFIG_VANDAL_UART_BAUD_RATE);
        s_hw_init = true;
        xTaskCreate(uart_master_task, "uart_master", 4096, NULL, 5, &s_task_handle);
    }
    vandal_proto_set_running(VANDAL_PROTO_UART, true);
    ESP_LOGI(TAG, "Started");
}

void vandal_uart_stop(void)
{
    if (!vandal_is_master())
        return; /* no-op on slave */

    vandal_proto_set_running(VANDAL_PROTO_UART, false);
    ESP_LOGI(TAG, "Stopped");
}
