#include "token_manager.h"
#include "sync.h"
#include "ubus.h"
#include "core/console.h"
#include <string.h>
#include <time.h>

static Console csl = {
    .topic = "token-mgr",
};

// Check if the current access token is still valid
bool sync_is_token_valid(struct ConfigSyncContext *context) {
    if (!context || !context->token_initialized) {
        return false;
    }
    
    if (strlen(context->access_token) == 0) {
        return false;
    }
    
    time_t now = time(NULL);
    if (now >= context->token_expiry) {
        console_debug(&csl, "Token expired: now=%ld, expiry=%ld", now, context->token_expiry);
        return false;
    }
    
    return true;
}

// Refresh access token
int sync_refresh_access_token(struct ConfigSyncContext *context) {
    if (!context) {
        console_error(&csl, "Invalid context for token refresh");
        return -1;
    }

    console_info(&csl, "Refreshing access token via UBUS...");

    // Check if UBUS is available for token requests
    if (!ubus_is_available_for_tokens()) {
        console_debug(&csl, "UBUS not available for token requests (fry-agent not ready)");
        return -1; 
    }

    char token_buffer[256];
    time_t expiry;

    // Use existing UBUS function to get access token
    int ret = ubus_get_access_token_sync(token_buffer, sizeof(token_buffer), &expiry);
    if (ret < 0) {
        console_error(&csl, "Failed to refresh access token via UBUS");
        return ret;
    }

    strncpy(context->access_token, token_buffer, sizeof(context->access_token) - 1);
    context->access_token[sizeof(context->access_token) - 1] = '\0';
    context->token_expiry = expiry;
    context->token_initialized = true;

    console_info(&csl, "Access token refreshed successfully via UBUS");
    return 0;
}

// Get current access token for HTTP requests
const char *sync_get_current_token(struct ConfigSyncContext *context) {
    if (!context || !context->token_initialized) {
        return NULL;
    }
    
    if (!sync_is_token_valid(context)) {
        return NULL;
    }
    
    return context->access_token;
}
