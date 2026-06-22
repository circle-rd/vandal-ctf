/*
 * Vandal Test - Thread (OpenThread) module.
 */

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Initialize OpenThread stack (slave: called at boot).
     */
    void vandal_thread_init(void);

    /**
     * @brief Start Thread TX (master only, lazy OT init).
     */
    void vandal_thread_start(void);

    /**
     * @brief Stop Thread TX (master only).
     */
    void vandal_thread_stop(void);

#ifdef __cplusplus
}
#endif
