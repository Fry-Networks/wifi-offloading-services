#include "core/console.h"
#include "core/uloop_scheduler.h"
#include "sync/sync.h"
#include "sync/token_manager.h"
#include "config.h"
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

static Console csl = {
    .topic = "config-main",
};

static ConfigSyncContext *sync_context = NULL;
static bool dev_env = false;

static void token_refresh_task_cb(void *ctx) {
    ConfigSyncContext *context = (ConfigSyncContext *)ctx;
    
    if (!context) {
        console_error(&csl, "Token refresh called with null context");
        return;
    }
    
    console_debug(&csl, "Checking token validity...");

    // Verify if UBUS is available
    if (!ubus_is_available_for_tokens()) {
        console_debug(&csl, "UBUS not connected, skipping token refresh");
        return;
    }

    // Check and refresh token if needed
    if (!sync_is_token_valid(context)) {
        console_info(&csl, "Access token expired or invalid, refreshing...");
        
        int ret = sync_refresh_access_token(context);
        if (ret < 0) {
            console_warn(&csl, "Failed to refresh access token: %d", ret);
        } else {
            console_info(&csl, "Access token refreshed successfully");
        }
    } else {
        console_debug(&csl, "Access token still valid");
    }
}

static void cleanup(void) {
    if (sync_context) {
        clean_config_sync_context(sync_context);
        sync_context = NULL;
    }
}

static bool process_command_line_args(int argc, char *argv[]) {
    dev_env = false;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dev") == 0) {
            dev_env = true;
        }
    }
    return true;
}

int main(int argc, char *argv[]) {
    console_set_syslog_facility(CONSOLE_FACILITY_DAEMON);
    console_set_channels(CONSOLE_CHANNEL_SYSLOG | CONSOLE_CHANNEL_STDIO);
    console_set_identity("fry-config");

    if (!process_command_line_args(argc, argv)) {
        return 0;
    }

    console_info(&csl, "Starting fry-config service");

    const remote_config_t *config = config_get_current();
    if (!config) {
        console_error(&csl, "Failed to load configuration");
        return 1;
    }

    if (!config_is_enabled()) {
        console_info(&csl, "Configuration service is disabled");
        return 0;
    }

    if (config->config_loaded) {
        console_set_level(config->console_log_level);
        console_info(&csl, "Console log level set to %d", config->console_log_level);
    }

    if (dev_env) {
        console_info(&csl, "fry-config started in DEVELOPMENT mode");
    } else {
        console_info(&csl, "fry-config service started");
    }

    scheduler_init();
    console_info(&csl, "uloop scheduler initialized");

    const char *endpoint = config_get_config_endpoint();
    if (!endpoint) {
        console_error(&csl, "No config endpoint configured");
        cleanup();
        return 1;
    }
    
    console_info(&csl, "Using config endpoint: %s", endpoint);

    uint32_t config_interval_ms = config_get_config_interval_ms();
    uint32_t initial_delay = dev_env ? 5000 : 5000;  
    uint32_t interval = config_interval_ms; 

    console_info(&csl, "Using config interval: %u ms (%u seconds)", 
                 config_interval_ms, config_interval_ms / 1000);

    sync_context = start_config_sync_service(endpoint, initial_delay, interval, dev_env);
    if (!sync_context) {
        console_error(&csl, "Failed to start config sync service");
        cleanup();
        return 1;
    }

    console_info(&csl, "Scheduling token refresh timer");
    task_id_t token_task = schedule_repeating(1000, 10000, token_refresh_task_cb, sync_context);
    if (token_task == 0) {
        console_warn(&csl, "Failed to schedule token refresh timer");
    } else {
        console_info(&csl, "Token refresh timer scheduled - initial check in 1 second, then every 10 seconds");
    }

    console_info(&csl, "Config sync service started successfully");
    console_info(&csl, "Starting event loop");
    console_info(&csl, "Services scheduled, starting scheduler main loop");
    
    int scheduler_result = scheduler_run();
    
    console_info(&csl, "Scheduler main loop ended with result: %d", scheduler_result);
    console_info(&csl, "Shutting down config service...");

    cleanup();
    console_info(&csl, "Config service stopped");
    return 0;
}