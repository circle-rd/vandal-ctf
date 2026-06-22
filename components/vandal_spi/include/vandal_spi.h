/*
 * Vandal Test - SPI module header.
 *
 * Master: periodically sends payload over SPI.
 * Slave:  listens on SPI, posts received payload as event.
 *
 * When stacked the same physical lines are shared; the slave uses the
 * SPI slave driver on the same GPIOs.
 */

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Initialize and start the SPI module (slave: called at boot).
     */
    void vandal_spi_init(void);

    /**
     * @brief Start SPI transmission (master only, lazy HW init).
     */
    void vandal_spi_start(void);

    /**
     * @brief Stop SPI transmission (master only).
     */
    void vandal_spi_stop(void);

#ifdef __cplusplus
}
#endif
