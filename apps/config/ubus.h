#ifndef UBUS_H
#define UBUS_H

#include <libubus.h>
#include <stdbool.h>
#include <time.h>

/**
 * UBUS Token Management Functions
 * These functions handle communication with fry-agent for access tokens
 */

/**
 * Get access token from fry-agent via UBUS (synchronous)
 * @param token_buf Buffer to store the token (should be at least 256 bytes)
 * @param token_size Size of the token buffer  
 * @param expiry Pointer to store token expiry time
 * @return 0 on success, negative error code on failure
 */
int ubus_get_access_token_sync(char *token_buf, size_t token_size, time_t *expiry);

/**
 * Check if UBUS is available for token requests
 * @return true if UBUS can be used and fry-agent is available, false otherwise
 */
bool ubus_is_available_for_tokens(void);

/**
 * Get device information from fry-agent via UBUS (synchronous)
 * @param name_buf Buffer to store device name (codename)
 * @param name_size Size of name buffer
 * @param model_buf Buffer to store device model
 * @param model_size Size of model buffer
 * @return 0 on success, negative on error
 */
int ubus_get_device_info_sync(char *name_buf, size_t name_size, char *model_buf, size_t model_size);

#endif // UBUS_H