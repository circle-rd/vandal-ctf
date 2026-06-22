/*
 * Vandal Test - I2C module header.
 *
 * Master: periodically writes payload to I2C slave address.
 * Slave:  listens as I2C slave, posts received payload as event.
 *
 * Both boards share the same SDA/SCL physical lines (GPIO22/23).
 * I2C naturally supports master/slave on a shared bus.
 */

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Initialize and start the I2C module (slave: called at boot).
     */
    void vandal_i2c_init(void);

    /**
     * @brief Start I2C transmission (master only, lazy HW init).
     */
    void vandal_i2c_start(void);

    /**
     * @brief Stop I2C transmission (master only).
     */
    void vandal_i2c_stop(void);

#ifdef __cplusplus
}
#endif
