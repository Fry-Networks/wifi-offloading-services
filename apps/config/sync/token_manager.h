#ifndef TOKEN_MANAGER_H
#define TOKEN_MANAGER_H

#include <stdbool.h>
#include <time.h>

// Forward declaration to avoid circular dependencies
struct ConfigSyncContext;

/**
 * Token Management Functions for fry-config
 * These functions handle access token lifecycle and validation
 */

/**
 * Check if the current access token is still valid
 * @param context Sync context containing token information
 * @return true if token is valid and not expired, false otherwise
 */
bool sync_is_token_valid(struct ConfigSyncContext *context);

/**
 * Refresh access token from fry-agent via UBUS
 * @param context Sync context to update with new token
 * @return 0 on success, negative error code on failure
 */
int sync_refresh_access_token(struct ConfigSyncContext *context);

/**
 * Get current access token for HTTP requests
 * @param context Sync context containing token
 * @return pointer to token string or NULL if invalid/expired
 */
const char *sync_get_current_token(struct ConfigSyncContext *context);

/**
 * Set whether the sync should accept new HTTP requests
 * @param context Sync context to update
 * @param accept true to enable requests, false to disable
 */
void sync_set_request_acceptance(struct ConfigSyncContext *context, bool accept);

/**
 * Check if the sync should accept new HTTP requests
 * @param context Sync context to check
 * @return true if requests should be accepted, false otherwise
 */
bool sync_should_accept_requests(struct ConfigSyncContext *context);

/**
 * Report HTTP request failure for network monitoring
 * @param context Sync context to update failure counter
 * @param error_code Error code from HTTP request
 */
void sync_report_http_failure(struct ConfigSyncContext *context, int error_code);

/**
 * Report successful HTTP request to reset failure counter
 * @param context Sync context to reset failure counter
 */
void sync_report_http_success(struct ConfigSyncContext *context);

#endif /* TOKEN_MANAGER_H */