/*
 * Vandal Test - UART module header.
 *
 * Master: periodically sends payload over UART.
 * Slave:  listens on UART, posts received payload as event.
 *
 * Pin remapping strategy for stacked boards:
 *   Master TX (GPIO16) ─── physical pin ─── Slave RX (remapped to GPIO16)
 *   Master RX (GPIO17) ─── physical pin ─── Slave TX (remapped to GPIO17)
 */

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Initialize and start the UART module (slave: called at boot).
     */
    void vandal_uart_init(void);

    /**
     * @brief Start UART transmission (master only, lazy HW init).
     */
    void vandal_uart_start(void);

    /**
     * @brief Stop UART transmission (master only).
     */
    void vandal_uart_stop(void);

#ifdef __cplusplus
}
#endif
