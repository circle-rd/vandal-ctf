/*
 * Vandal Test - HTTP module header.
 *
 * Master: HTTP server on the open AP.
 *   - POST /messages : receives JSON status reports from slave.
 *   - GET  /         : serves static files (placeholder for now).
 *
 * Slave:  HTTP client that periodically POSTs a JSON report
 *         of all received protocol payloads to the master.
 *
 * Must be called AFTER vandal_wifi_init() (needs network up).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the HTTP module.
 *
 * On master: starts the HTTP server.
 * On slave:  starts a background task that POSTs protocol
 *            payloads to the master server periodically.
 */
void vandal_http_init(void);

#ifdef __cplusplus
}
#endif
