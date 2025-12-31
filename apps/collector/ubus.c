#include "ubus.h"
#include "collect.h"
#include "config.h"
#include "core/console.h"
#include <libubox/blobmsg.h>
#include <libubox/blobmsg_json.h>
#include <libubox/ustream.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#define UBUS_RECONNECT_DELAY_MS 1000
#define UBUS_RECONNECT_MAX_TRIES 10

static Console csl = {
    .topic = "ubus",
};

// UBUS connection
static struct ubus_context *ctx = NULL;
static bool ubus_connected = false;
static int reconnect_tries = UBUS_RECONNECT_MAX_TRIES;

// Log reading state
static struct ustream_fd log_stream;
static struct ubus_request log_request;
static bool log_streaming = false;

// Timers
static struct uloop_timeout reconnect_timer;
static struct uloop_timeout token_refresh_timer;

// Log policy
enum { LOG_MSG, LOG_ID, LOG_PRIO, LOG_SOURCE, LOG_TIME, __LOG_MAX };

static const struct blobmsg_policy log_policy[] = {
    [LOG_MSG] = {.name = "msg", .type = BLOBMSG_TYPE_STRING},
    [LOG_ID] = {.name = "id", .type = BLOBMSG_TYPE_INT32},
    [LOG_PRIO] = {.name = "priority", .type = BLOBMSG_TYPE_INT32},
    [LOG_SOURCE] = {.name = "source", .type = BLOBMSG_TYPE_INT32},
    [LOG_TIME] = {.name = "time", .type = BLOBMSG_TYPE_INT64},
};

// Access token management
static char access_token[256] = {0};
static time_t token_expiry = 0;
static bool token_initialized = false;

// Log acceptance control
static bool accept_logs = false;
static int consecutive_network_failures = 0;
static const int MAX_NETWORK_FAILURES = 3;

// Forward declarations
static void start_log_streaming(void);
static void stop_log_streaming(void);

/**
 * Check if log entry should be processed based on filters
 */
static bool should_process_log(const char *msg, int priority, int source) {
    // Example: filter out debug messages
    if ((priority & 0x7) == LOG_DEBUG) {
        return false;
    }

    // Example: filter out kernel messages if needed
    // if (source == 0) { // SOURCE_KLOG
    //     return false;
    // }

    // Add more filtering logic as needed
    return true;
}

/**
 * Process a single log entry
 */
static void process_log_entry(struct blob_attr *tb[__LOG_MAX]) {
    const char *msg;
    uint32_t priority, source;
    uint64_t timestamp;

    // Short circuit: Don't accept logs if token is not available or network issues
    if (!accept_logs) {
        return;
    }

    msg = blobmsg_get_string(tb[LOG_MSG]);
    priority = blobmsg_get_u32(tb[LOG_PRIO]);
    source = blobmsg_get_u32(tb[LOG_SOURCE]);
    timestamp = blobmsg_get_u64(tb[LOG_TIME]);

    // Apply filters
    if (!should_process_log(msg, priority, source)) {
        return;
    }

    // Create log structure for collection with raw fields
    log_data_t log_data = {.time = timestamp,
                           .priority = priority,
                           .source = source,
                           .msg = msg};

    // Enqueue the log
    int ret = collect_enqueue_log(&log_data);
    if (ret < 0) {
        console_warn(&csl, "Failed to enqueue log: %d", ret);
    }
}

/**
 * Handle incoming log data from the stream
 */
static void log_stream_data_cb(struct ustream *s, int bytes) {
    while (true) {
        struct blob_attr *a;
        struct blob_attr *tb[__LOG_MAX];
        int len, cur_len;

        // Get available data
        a = (void *)ustream_get_read_buf(s, &len);
        if (len < (int)sizeof(*a)) break;

        // Check if we have a complete message
        cur_len = blob_len(a) + sizeof(*a);
        if (len < cur_len) break;

        // Parse the log entry
        if (blobmsg_parse(log_policy, ARRAY_SIZE(log_policy), tb, blob_data(a), blob_len(a)) == 0) {
            // Verify all required fields are present
            if (tb[LOG_ID] && tb[LOG_PRIO] && tb[LOG_SOURCE] && tb[LOG_TIME] && tb[LOG_MSG]) {
                process_log_entry(tb);
            }
        }

        // Consume the processed message
        ustream_consume(s, cur_len);
    }
}

/**
 * Handle stream state changes
 */
static void log_stream_state_cb(struct ustream *s) {
    console_info(&csl, "Log stream state changed, EOF=%d", s->eof);

    if (s->eof) {
        // Stream ended, clean up and try to reconnect
        stop_log_streaming();

        // Schedule reconnection
        if (reconnect_tries > 0) {
            uloop_timeout_set(&reconnect_timer, UBUS_RECONNECT_DELAY_MS);
        }
    }
}

/**
 * File descriptor callback for log reading
 */
static void log_read_fd_cb(struct ubus_request *req, int fd) {
    console_info(&csl, "Got log stream file descriptor: %d", fd);

    // Initialize ustream for the file descriptor
    memset(&log_stream, 0, sizeof(log_stream));
    log_stream.stream.notify_read = log_stream_data_cb;
    log_stream.stream.notify_state = log_stream_state_cb;
    ustream_fd_init(&log_stream, fd);

    log_streaming = true;
}

/**
 * Connection lost callback
 */
static void ubus_connection_lost_cb(struct ubus_context *ctx) {
    console_warn(&csl, "UBUS connection lost");
    ubus_connected = false;

    // Stop log streaming
    stop_log_streaming();

    // Schedule reconnection
    reconnect_tries = UBUS_RECONNECT_MAX_TRIES;
    uloop_timeout_set(&reconnect_timer, UBUS_RECONNECT_DELAY_MS);
}

/**
 * Stop log streaming
 */
static void stop_log_streaming(void) {
    if (log_streaming) {
        console_info(&csl, "Stopping log stream");
        ustream_free(&log_stream.stream);
        log_streaming = false;
    }
}

/**
 * Start log streaming
 */
static void start_log_streaming(void) {
    uint32_t id;
    int ret;
    static struct blob_buf b;

    if (!ctx || !ubus_connected || log_streaming) {
        return;
    }

    // Only start streaming if we should accept logs
    if (!accept_logs) {
        console_info(&csl, "Not starting log streaming - log acceptance disabled");
        return;
    }

    // Look up the log object
    ret = ubus_lookup_id(ctx, "log", &id);
    if (ret != 0) {
        console_error(&csl, "Failed to find log object: %s", ubus_strerror(ret));
        return;
    }

    // Prepare request
    blob_buf_init(&b, 0);
    blobmsg_add_u8(&b, "stream", 1);  // Enable streaming
    blobmsg_add_u8(&b, "oneshot", 0); // Continuous streaming (like -f)
    blobmsg_add_u32(&b, "lines", 0);  // Start from current position

    // Make async request
    memset(&log_request, 0, sizeof(log_request));
    ret = ubus_invoke_async(ctx, id, "read", b.head, &log_request);
    if (ret != 0) {
        console_error(&csl, "Failed to invoke log read: %s", ubus_strerror(ret));
        blob_buf_free(&b);
        return;
    }

    // Set file descriptor callback
    log_request.fd_cb = log_read_fd_cb;

    // Complete the async request
    ubus_complete_request_async(ctx, &log_request);

    console_info(&csl, "Started log streaming");
    blob_buf_free(&b);
}

/**
 * Reconnect timer callback
 */
static void reconnect_timer_cb(struct uloop_timeout *timeout) {
    if (reconnect_tries <= 0) {
        console_error(&csl, "Maximum reconnection attempts reached");
        return;
    }

    reconnect_tries--;
    console_info(&csl, "Attempting to reconnect to UBUS (tries left: %d)", reconnect_tries);

    // Clean up old context
    if (ctx) {
        ubus_free(ctx);
        ctx = NULL;
    }

    // Try to connect
    const char *ubus_socket = config_get_current()->dev_mode ? "/tmp/ubus.sock" : NULL;
    ctx = ubus_connect(ubus_socket);
    if (!ctx) {
        console_error(&csl, "Failed to connect to UBUS");
        uloop_timeout_set(&reconnect_timer, UBUS_RECONNECT_DELAY_MS);
        return;
    }

    // Set connection lost handler
    ctx->connection_lost = ubus_connection_lost_cb;

    // Add to uloop
    ubus_add_uloop(ctx);
    ubus_connected = true;

    console_info(&csl, "Reconnected to UBUS");

    // Start log streaming
    start_log_streaming();
}

/**
 * Initialize UBUS connection
 */
int ubus_init(void) {
    const char *ubus_socket = config_get_current()->dev_mode ? "/tmp/ubus.sock" : NULL;

    console_info(&csl, "Initializing UBUS connection");

    // Connect to UBUS
    ctx = ubus_connect(ubus_socket);
    if (!ctx) {
        console_error(&csl, "Failed to connect to UBUS");
        return -1;
    }

    // Set connection lost handler
    ctx->connection_lost = ubus_connection_lost_cb;

    // Add to uloop
    ubus_add_uloop(ctx);
    ubus_connected = true;

    // Initialize timers
    reconnect_timer.cb = reconnect_timer_cb;

    console_info(&csl, "UBUS initialized successfully");

    // Don't start log streaming yet - wait for token initialization
    console_info(&csl, "Waiting for access token before starting log streaming");

    return 0;
}

/**
 * Start the UBUS loop (not needed with uloop integration)
 */
int ubus_start_loop(void) {
    // Nothing to do - uloop handles everything
    return 0;
}

/**
 * Cleanup UBUS connection
 */
void ubus_cleanup(void) {
    console_info(&csl, "Cleaning up UBUS");

    // Cancel timers
    uloop_timeout_cancel(&reconnect_timer);
    uloop_timeout_cancel(&token_refresh_timer);

    // Stop log streaming
    stop_log_streaming();

    // Free UBUS context
    if (ctx) {
        ubus_free(ctx);
        ctx = NULL;
    }

    ubus_connected = false;
    console_info(&csl, "UBUS cleanup complete");
}

/**
 * Check if UBUS is connected
 */
bool ubus_is_connected(void) { return ubus_connected && ctx != NULL; }

/**
 * Check if the collector should accept new logs
 */
bool ubus_should_accept_logs(void) {
    return accept_logs;
}

/**
 * Set whether the collector should accept new logs
 */
void ubus_set_log_acceptance(bool accept) {
    if (accept_logs != accept) {
        console_info(&csl, "Log acceptance %s", accept ? "enabled" : "disabled");
        accept_logs = accept;
        
        if (accept && !log_streaming) {
            // Start streaming if enabling acceptance
            start_log_streaming();
        } else if (!accept && log_streaming) {
            // Stop streaming if disabling acceptance
            stop_log_streaming();
        }
    }
}

/**
 * Get current access token for HTTP requests
 */
const char *ubus_get_current_token(void) {
    if (!token_initialized || !ubus_is_access_token_valid()) {
        return NULL;
    }
    return access_token;
}

/**
 * Report network connectivity issues to stop log acceptance
 */
void ubus_report_network_failure(int consecutive_failures) {
    consecutive_network_failures = consecutive_failures;
    
    if (consecutive_failures >= MAX_NETWORK_FAILURES && accept_logs) {
        console_warn(&csl, "Too many network failures (%d), disabling log acceptance", consecutive_failures);
        ubus_set_log_acceptance(false);
    } else if (consecutive_failures == 0 && !accept_logs && token_initialized && ubus_is_access_token_valid()) {
        console_info(&csl, "Network recovered and token available, enabling log acceptance");
        ubus_set_log_acceptance(true);
    }
}

/**
 * Structure to hold response data
 */
struct token_response_data {
    struct blob_buf buf;
    bool received;
};

/**
 * Sync response callback for access token
 */
static void ubus_sync_response_cb(struct ubus_request *req, int type, struct blob_attr *msg) {
    struct token_response_data *data = (struct token_response_data *)req->priv;

    console_debug(&csl, "Sync response callback: type=%d, msg=%p, data=%p", type, msg, data);

    if (type == UBUS_MSG_DATA && msg && data) {
        blob_buf_init(&data->buf, 0);
        blob_put_raw(&data->buf, blob_data(msg), blob_len(msg));
        data->received = true;
        console_debug(&csl, "Response message copied to buffer");
    }
}

/**
 * Get access token from fry-agent
 */
int ubus_get_access_token(char *token_buf, size_t token_size, time_t *expiry) {
    uint32_t id;
    struct blob_buf b = {};
    struct token_response_data response_data = {.received = false};
    int ret;

    if (!ctx || !ubus_connected) {
        console_error(&csl, "UBUS not connected");
        return -1;
    }

    if (!token_buf || token_size < 2 || !expiry) {
        console_error(&csl, "Invalid parameters");
        return -1;
    }

    // Look up fry-agent object
    ret = ubus_lookup_id(ctx, "fry-agent", &id);
    if (ret != 0) {
        console_error(&csl, "Failed to find fry-agent object: %s", ubus_strerror(ret));
        return -1;
    }

    console_debug(&csl, "Found fry-agent object with id: %u", id);

    // Prepare request
    blob_buf_init(&b, 0);

    // Invoke get_access_token method
    ret = ubus_invoke(ctx, id, "get_access_token", b.head, ubus_sync_response_cb, &response_data, 5000);

    blob_buf_free(&b);

    if (ret != 0) {
        console_error(&csl, "Failed to get access token: %s", ubus_strerror(ret));
        return -1;
    }

    if (!response_data.received || !response_data.buf.head) {
        console_error(&csl, "No response received from fry-agent");
        return -1;
    }

    // Log the raw response for debugging
    char *json_str = blobmsg_format_json(response_data.buf.head, true);
    if (json_str) {
        console_debug(&csl, "Raw token response: %s", json_str);
        free(json_str);
    }

    // Parse the response
    enum { TOKEN_FIELD, ISSUED_AT_FIELD, EXPIRES_AT_FIELD, VALID_FIELD, __TOKEN_MAX };

    static const struct blobmsg_policy token_policy[__TOKEN_MAX] = {
        [TOKEN_FIELD] = {.name = "token", .type = BLOBMSG_TYPE_STRING},
        [ISSUED_AT_FIELD] = {.name = "issued_at", .type = BLOBMSG_TYPE_INT64},
        [EXPIRES_AT_FIELD] = {.name = "expires_at", .type = BLOBMSG_TYPE_INT64},
        [VALID_FIELD] = {.name = "valid", .type = BLOBMSG_TYPE_INT8},
    };

    struct blob_attr *tb[__TOKEN_MAX];

    if (blobmsg_parse(token_policy, __TOKEN_MAX, tb, blobmsg_data(response_data.buf.head),
                      blobmsg_len(response_data.buf.head)) != 0) {
        console_error(&csl, "Failed to parse token response");
        blob_buf_free(&response_data.buf);
        return -1;
    }

    // Log which fields were found
    console_debug(&csl, "Token field present: %s", tb[TOKEN_FIELD] ? "yes" : "no");
    console_debug(&csl, "Issued_at field present: %s", tb[ISSUED_AT_FIELD] ? "yes" : "no");
    console_debug(&csl, "Expires_at field present: %s", tb[EXPIRES_AT_FIELD] ? "yes" : "no");
    console_debug(&csl, "Valid field present: %s", tb[VALID_FIELD] ? "yes" : "no");

    // Check if all required fields are present
    if (!tb[TOKEN_FIELD] || !tb[EXPIRES_AT_FIELD] || !tb[VALID_FIELD]) {
        console_error(&csl, "Missing required fields in token response");
        blob_buf_free(&response_data.buf);
        return -1;
    }

    // Check if token is valid
    uint8_t valid = blobmsg_get_u8(tb[VALID_FIELD]);
    console_debug(&csl, "Token valid field: %s", valid ? "true" : "false");
    if (!valid) {
        console_error(&csl, "Token marked as invalid by fry-agent");
        blob_buf_free(&response_data.buf);
        return -1;
    }

    // Extract token
    const char *token = blobmsg_get_string(tb[TOKEN_FIELD]);
    if (!token || strlen(token) == 0) {
        console_error(&csl, "Empty token received");
        blob_buf_free(&response_data.buf);
        return -1;
    }

    console_debug(&csl, "Token length: %zu", strlen(token));

    // Copy token to buffer
    size_t token_len = strlen(token);
    if (token_len >= token_size) {
        console_error(&csl, "Token too large for buffer (token: %zu, buffer: %zu)", token_len, token_size);
        blob_buf_free(&response_data.buf);
        return -1;
    }

    strncpy(token_buf, token, token_size - 1);
    token_buf[token_size - 1] = '\0';

    // Extract expiry time
    *expiry = (time_t)blobmsg_get_u64(tb[EXPIRES_AT_FIELD]);

    console_info(&csl, "Successfully retrieved access token, expires at %ld", *expiry);

    // Clean up
    blob_buf_free(&response_data.buf);

    return 0;
}

/**
 * Check if access token is still valid
 */
bool ubus_is_access_token_valid(void) {
    if (strlen(access_token) == 0) {
        return false;
    }

    return time(NULL) < token_expiry;
}

/**
 * Refresh access token
 */
int ubus_refresh_access_token(void) {
    time_t new_expiry;
    char new_token[256];

    int ret = ubus_get_access_token(new_token, sizeof(new_token), &new_expiry);
    if (ret < 0) {
        console_error(&csl, "Failed to refresh access token");
        return ret;
    }

    // Update global token
    strncpy(access_token, new_token, sizeof(access_token));
    token_expiry = new_expiry;
    token_initialized = true;

    // Reset network failure counter on successful token refresh
    consecutive_network_failures = 0;

    // Enable log acceptance if token is valid and no network issues
    bool should_accept = (consecutive_network_failures < MAX_NETWORK_FAILURES);
    if (should_accept) {
        if (!accept_logs) {
            console_info(&csl, "Enabling log acceptance - token available and network healthy");
            ubus_set_log_acceptance(true);
        }
    } else {
        console_warn(&csl, "Token available but network issues detected, keeping log acceptance disabled");
    }

    console_info(&csl, "Access token refreshed successfully");
    return 0;
}
