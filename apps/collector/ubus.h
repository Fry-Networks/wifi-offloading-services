#ifndef UBUS_H
#define UBUS_H

#include <libubus.h>
#include <stdbool.h>
#include <time.h>

/**
 * Initialize UBUS connection and subscribe to syslog events
 * @return 0 on success, negative error code on failure
 */
int ubus_init(void);

/**
 * Start the UBUS event loop in a separate thread
 * This function will block and handle UBUS events
 * @return 0 on success, negative error code on failure
 */
int ubus_start_loop(void);

/**
 * Stop the UBUS event loop and cleanup
 */
void ubus_cleanup(void);

/**
 * Check if UBUS connection is active
 * @return true if connected, false otherwise
 */
bool ubus_is_connected(void);

/**
 * Retrieve access token from fry-agent via UBUS
 * @param token_buf Buffer to store the token (should be at least 256 bytes)
 * @param token_size Size of the token buffer
 * @param expiry Pointer to store token expiry time
 * @return 0 on success, negative error code on failure
 */
int ubus_get_access_token(char *token_buf, size_t token_size, time_t *expiry);

/**
 * Check if the cached access token is still valid
 * @return true if valid, false if expired or not available
 */
bool ubus_is_access_token_valid(void);

/**
 * Force refresh of the cached access token from fry-agent
 * @return 0 on success, negative error code on failure
 */
int ubus_refresh_access_token(void);

/**
 * Check if the collector should accept new logs
 * @return true if logs should be accepted, false otherwise
 */
bool ubus_should_accept_logs(void);

/**
 * Set whether the collector should accept new logs
 * @param accept true to accept logs, false to reject them
 */
void ubus_set_log_acceptance(bool accept);

/**
 * Get current access token for HTTP requests
 * @return pointer to current token string, or NULL if not available
 */
const char *ubus_get_current_token(void);

/**
 * Report network connectivity issues to stop log acceptance
 * @param consecutive_failures Number of consecutive HTTP failures
 */
void ubus_report_network_failure(int consecutive_failures);

#endif // UBUS_H