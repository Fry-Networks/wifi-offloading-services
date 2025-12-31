#include "collect.h"
#include "config.h"
#include "core/console.h"
#include "ubus.h"
#include <libubox/uloop.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <unistd.h>

static Console csl = {
    .topic = "collector",
};

static volatile bool running = true;
static bool dev_env = false;

// Timer for batch processing
static struct uloop_timeout batch_timer;
static struct uloop_timeout status_timer;
static struct uloop_timeout token_refresh_timer;

/**
 * Signal handler for graceful shutdown
 */
static void signal_handler(int sig) {
    console_info(&csl, "Received signal %d, shutting down...", sig);
    running = false;
    uloop_end();
}

/**
 * Batch processing timer callback
 */
static void batch_timer_cb(struct uloop_timeout *timeout) {
    if (!running) return;

    // Process any pending batches
    collect_process_pending_batches();

    // Schedule next batch check
    uloop_timeout_set(&batch_timer, 1000); // Check every second
}

/**
 * Status monitoring timer callback
 */
static void status_timer_cb(struct uloop_timeout *timeout) {
    if (!running) return;

    uint32_t queue_size, dropped_count;
    if (collect_get_stats(&queue_size, &dropped_count) == 0) {
        if (dev_env) {
            console_info(&csl, "Status: queue_size=%u, dropped=%u, ubus_connected=%s", queue_size, dropped_count,
                         ubus_is_connected() ? "yes" : "no");
        }

        // Warn if queue is getting full
        uint32_t urgent_threshold = config_get_queue_size() * 80 / 100;
        if (queue_size > urgent_threshold) {
            console_warn(&csl, "Log queue getting full: %u entries (threshold: %u)", queue_size, urgent_threshold);
        }

        // Warn about dropped logs
        if (dropped_count > 0) {
            console_warn(&csl, "Dropped %u log entries due to full queue", dropped_count);
        }
    }

    // Schedule next status check
    uloop_timeout_set(&status_timer, 30000); // Every 30 seconds
}

/**
 * Access token refresh timer callback
 */
static void token_refresh_timer_cb(struct uloop_timeout *timeout) {
    // Defensive check - should not happen but prevents crashes
    if (!timeout) {
        console_error(&csl, "Token refresh timer callback called with null timeout");
        return;
    }

    if (!running) {
        console_debug(&csl, "Service not running, skipping token refresh");
        return;
    }

    // Check if UBUS is available before attempting token operations
    if (!ubus_is_connected()) {
        console_warn(&csl, "UBUS not connected, skipping token refresh");
        // Schedule next check with shorter interval to retry sooner
        uloop_timeout_set(&token_refresh_timer, 10000); // 10 second retry when UBUS down
        return;
    }

    // Check if current token is still valid
    bool token_valid = ubus_is_access_token_valid();
    bool currently_accepting = ubus_should_accept_logs();

    if (!token_valid) {
        console_info(&csl, "Access token expired or invalid, refreshing...");

        // Attempt token refresh with error handling
        int ret = ubus_refresh_access_token();
        if (ret < 0) {
            console_warn(&csl, "Failed to refresh access token: %d", ret);

            // Disable log acceptance if token refresh fails
            if (currently_accepting) {
                console_warn(&csl, "Disabling log acceptance due to token refresh failure");
                ubus_set_log_acceptance(false);
            }

            // On failure, schedule shorter retry interval based on whether we had a token before
            uint32_t retry_interval = currently_accepting ? 60000 : 10000; // 1 minute if we had token, 10 seconds for initial acquisition
            console_info(&csl, "Scheduling token refresh retry in %u ms", retry_interval);
            uloop_timeout_set(&token_refresh_timer, retry_interval);
            return;
        } else {
            console_info(&csl, "Access token refreshed successfully");
            // Token refresh function handles enabling log acceptance
        }
    } else if (dev_env) {
        console_debug(&csl, "Access token still valid");
    }

    // Schedule next token check (every 5 minutes for normal operation)
    uint32_t normal_interval = 300000; // 5 minutes
    if (dev_env) {
        console_debug(&csl, "Scheduling next token check in %u ms", normal_interval);
    }
    uloop_timeout_set(&token_refresh_timer, normal_interval);
}

/**
 * Process command line arguments
 */
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
    console_set_channels(CONSOLE_CHANNEL_SYSLOG);
    console_set_identity("fry-collector");

    int ret;

    // Process command line arguments first
    if (!process_command_line_args(argc, argv)) {
        return 0;
    }

    // Initialize configuration
    const collector_config_t *config = config_get_current();
    if (!config) {
        console_error(&csl, "Failed to load configuration");
        return 1;
    }

    // Validate configuration
    ret = config_validate(config);
    if (ret < 0) {
        console_error(&csl, "Configuration validation failed");
        return 1;
    }

    // Check if collector is enabled
    if (!config_is_enabled()) {
        console_info(&csl, "Collector is disabled in configuration");
        return 0;
    }

    // Initialize console logging
    console_set_level(config->console_log_level);

    // Set up signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize uloop
    uloop_init();

    // Initialize collection system
    ret = collect_init();
    if (ret < 0) {
        console_error(&csl, "Failed to initialize collection system: %d", ret);
        return 1;
    }

    // Initialize UBUS connection
    ret = ubus_init();
    if (ret < 0) {
        console_error(&csl, "Failed to initialize UBUS: %d", ret);
        collect_cleanup();
        return 1;
    }

    console_info(&csl, "Starting event loop");

    // Set up batch processing timer
    batch_timer.cb = batch_timer_cb;
    uloop_timeout_set(&batch_timer, 1000); // Start in 1 second

    // Set up status monitoring timer
    status_timer.cb = status_timer_cb;
    uloop_timeout_set(&status_timer, 30000); // First status check in 30 seconds

    // Set up access token refresh timer - try to get initial token immediately
    token_refresh_timer.cb = token_refresh_timer_cb;
    uloop_timeout_set(&token_refresh_timer, 1000); // First token check in 1 second

    console_info(&csl, "Collector service running with event-driven architecture");
    console_info(&csl, "Log streaming will start once access token is acquired");

    // Run the main event loop
    uloop_run();

    console_info(&csl, "Shutting down collector service...");

    // Cancel timers
    uloop_timeout_cancel(&batch_timer);
    uloop_timeout_cancel(&status_timer);
    uloop_timeout_cancel(&token_refresh_timer);

    // Process any final batches
    collect_process_pending_batches();

    // Cleanup
    ubus_cleanup();
    collect_cleanup();
    uloop_done();

    console_info(&csl, "Collector service stopped");
    return 0;
}
