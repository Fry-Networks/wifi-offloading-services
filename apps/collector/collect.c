#include "collect.h"
#include "config.h"
#include "core/console.h"
#include "ubus.h"
#include <asm-generic/errno-base.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <json-c/json.h>
#include <libubox/uloop.h>

static Console csl = {
    .topic = "collect",
};

// Single-threaded state - no mutexes needed
static simple_log_queue_t queue;
static compact_log_entry_t *entry_pool = NULL;
static bool *pool_used = NULL;
static uint32_t entry_pool_size = 0;
static uint32_t dropped_count = 0;
static bool system_running = false;

// Batch processing state
static batch_context_t current_batch;
static time_t last_batch_time = 0;

// HTTP client state machine
static CURL *curl_handle = NULL;
static struct curl_slist *http_headers = NULL;
static char curl_error_buffer[CURL_ERROR_SIZE];

// Network failure tracking
static int consecutive_http_failures = 0;

// Configuration values are now obtained from config functions

/**
 * Initialize the entry pool for memory optimization
 */
static int init_entry_pool(void) {
    entry_pool_size = config_get_queue_size();

    entry_pool = calloc(entry_pool_size, sizeof(compact_log_entry_t));
    if (!entry_pool) {
        console_error(&csl, "Failed to allocate entry pool");
        return -ENOMEM;
    }

    pool_used = calloc(entry_pool_size, sizeof(bool));
    if (!pool_used) {
        console_error(&csl, "Failed to allocate pool tracking array");
        free(entry_pool);
        entry_pool = NULL;
        return -ENOMEM;
    }

    for (uint32_t i = 0; i < entry_pool_size; i++) {
        pool_used[i] = false;
        entry_pool[i].pool_index = i;
        entry_pool[i].in_use = false;
    }

    console_debug(&csl, "Entry pool initialized with %u entries", entry_pool_size);
    return 0;
}

/**
 * Get an entry from the pool
 */
compact_log_entry_t *collect_get_entry_from_pool(void) {
    if (!entry_pool || !pool_used) {
        return NULL;
    }

    for (uint32_t i = 0; i < entry_pool_size; i++) {
        if (!pool_used[i]) {
            pool_used[i] = true;
            entry_pool[i].in_use = true;
            return &entry_pool[i];
        }
    }
    return NULL; // Pool exhausted
}

/**
 * Return an entry to the pool
 */
void collect_return_entry_to_pool(compact_log_entry_t *entry) {
    if (!entry || !pool_used || entry->pool_index >= entry_pool_size) {
        return;
    }

    pool_used[entry->pool_index] = false;
    entry->in_use = false;
    memset(entry->msg, 0, sizeof(entry->msg));
    entry->priority = 0;
    entry->source = 0;
    entry->time = 0;
}

/**
 * Initialize the simple queue
 */
static int init_simple_queue(simple_log_queue_t *q) {
    uint32_t queue_size = config_get_queue_size();

    q->entries = calloc(queue_size, sizeof(compact_log_entry_t *));
    if (!q->entries) {
        console_error(&csl, "Failed to allocate queue entries array");
        return -ENOMEM;
    }

    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->max_size = queue_size;

    console_debug(&csl, "Queue initialized with %u entries", queue_size);
    return 0;
}

/**
 * Add entry to queue (single-threaded, no locks)
 */
static int enqueue_entry(simple_log_queue_t *q, compact_log_entry_t *entry) {
    if (q->count >= q->max_size) {
        return -1; // Queue full
    }

    q->entries[q->tail] = entry;
    q->tail = (q->tail + 1) % q->max_size;
    q->count++;

    return 0;
}

/**
 * Remove entry from queue (single-threaded, no locks)
 */
static compact_log_entry_t *dequeue_entry(simple_log_queue_t *q) {
    if (q->count == 0) {
        return NULL;
    }

    compact_log_entry_t *entry = q->entries[q->head];
    q->entries[q->head] = NULL;
    q->head = (q->head + 1) % q->max_size;
    q->count--;

    return entry;
}

/**
 * Initialize CURL for HTTP operations
 */
static int init_http_client(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    curl_handle = curl_easy_init();
    if (!curl_handle) {
        console_error(&csl, "Failed to initialize CURL");
        return -1;
    }

    // Set up common CURL options
    curl_easy_setopt(curl_handle, CURLOPT_ERRORBUFFER, curl_error_buffer);
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, (long)config_get_http_timeout());
    curl_easy_setopt(curl_handle, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl_handle, CURLOPT_SSL_VERIFYHOST, 2L);

    // Set up headers
    http_headers = curl_slist_append(http_headers, "Content-Type: application/json");
    http_headers = curl_slist_append(http_headers, "User-Agent: fry-collector/1.0");

    console_debug(&csl, "HTTP client initialized");
    return 0;
}

/**
 * Cleanup HTTP client
 */
static void cleanup_http_client(void) {
    if (http_headers) {
        curl_slist_free_all(http_headers);
        http_headers = NULL;
    }

    if (curl_handle) {
        curl_easy_cleanup(curl_handle);
        curl_handle = NULL;
    }

    curl_global_cleanup();
}

/**
 * Create JSON payload from batch entries
 */
static char *create_json_payload(compact_log_entry_t **entries, int count, size_t *payload_size) {
    json_object *root = json_object_new_object();
    json_object *logs_array = json_object_new_array();

    for (int i = 0; i < count; i++) {
        compact_log_entry_t *entry = entries[i];

        json_object *log_obj = json_object_new_object();
        json_object_object_add(log_obj, "msg", json_object_new_string(entry->msg));
        json_object_object_add(log_obj, "priority", json_object_new_int64(entry->priority));
        json_object_object_add(log_obj, "source", json_object_new_int64(entry->source));
        json_object_object_add(log_obj, "time", json_object_new_int64(entry->time));

        json_object_array_add(logs_array, log_obj);
    }

    json_object_object_add(root, "logs", logs_array);
    json_object_object_add(root, "count", json_object_new_int(count));
    json_object_object_add(root, "collector_version", json_object_new_string("1.0.0-raw-logs"));

    const char *json_string = json_object_to_json_string(root);
    *payload_size = strlen(json_string);
    char *payload = malloc(*payload_size + 1);

    if (payload) {
        strcpy(payload, json_string);
    }

    json_object_put(root);
    return payload;
}

/**
 * Send HTTP request (non-blocking approach)
 */
static int send_http_request(const char *payload, size_t payload_size) {
    if (!curl_handle || !payload) {
        return -1;
    }
    
    // Track request start time
    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    // Get cached access token
    const char *access_token = ubus_get_current_token();
    if (!access_token) {
        console_warn(&csl, "No valid access token available, aborting HTTP request");
        collect_report_http_failure(-1);
        return -1;
    }

    // Update headers with authorization
    struct curl_slist *request_headers = http_headers;
    char auth_header[600]; // Token + "Authorization: Bearer " prefix
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", access_token);
    request_headers = curl_slist_append(request_headers, auth_header);
    if (!request_headers) {
        console_error(&csl, "Failed to add authorization header");
        collect_report_http_failure(-1);
        return -1;
    }
    console_debug(&csl, "Added Bearer token to request");

    const char *backend_url = config_get_logs_endpoint();
    curl_easy_setopt(curl_handle, CURLOPT_URL, backend_url);
    curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl_handle, CURLOPT_POSTFIELDSIZE, payload_size);
    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, request_headers);

    CURLcode res = curl_easy_perform(curl_handle);

    // Clean up authorization header if we added it
    if (request_headers != http_headers) {
        curl_slist_free_all(request_headers);
    }

    if (res != CURLE_OK) {
        // Calculate duration for curl errors too
        struct timespec error_time;
        clock_gettime(CLOCK_MONOTONIC, &error_time);
        double error_duration_ms = (error_time.tv_sec - start_time.tv_sec) * 1000.0 + 
                                (error_time.tv_nsec - start_time.tv_nsec) / 1000000.0;
        
        console_warn(&csl, "HTTP request failed: %s - took %.2f ms", 
                    curl_error_buffer, error_duration_ms);
        collect_report_http_failure(-res);
        return -1;
    }

    long response_code;
    curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &response_code);

    // Calculate request duration
    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double duration_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 + 
                        (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;
    
    if (response_code >= 200 && response_code < 300) {
        console_info(&csl, "HTTP request successful (code: %ld) - took %.2f ms", 
                    response_code, duration_ms);
        collect_report_http_success();
        return 0;
    } else if (response_code == 401) {
        console_warn(&csl, "HTTP request failed with 401 Unauthorized, refreshing token - took %.2f ms", 
                    duration_ms);
        // Try to refresh the token for next request
        ubus_refresh_access_token();
        collect_report_http_failure(response_code);
        return -1;
    } else {
        console_warn(&csl, "HTTP request failed with code: %ld - took %.2f ms", 
                    response_code, duration_ms);
        collect_report_http_failure(response_code);
        return -1;
    }
}

/**
 * Initialize batch context
 */
static int init_batch_context(batch_context_t *ctx) {
    uint32_t batch_size = config_get_batch_size();

    ctx->entries = calloc(batch_size, sizeof(compact_log_entry_t *));
    if (!ctx->entries) {
        console_error(&csl, "Failed to allocate batch entries array");
        return -ENOMEM;
    }

    ctx->count = 0;
    ctx->max_count = batch_size;
    ctx->created_time = time(NULL);
    ctx->retry_count = 0;
    ctx->state = HTTP_IDLE;
    ctx->json_payload = NULL;
    ctx->payload_size = 0;

    console_debug(&csl, "Batch context initialized with %u entries", batch_size);
    return 0;
}

/**
 * Clear batch context and return entries to pool
 */
static void clear_batch_context(batch_context_t *ctx) {
    for (int i = 0; i < ctx->count; i++) {
        if (ctx->entries[i]) {
            collect_return_entry_to_pool(ctx->entries[i]);
            ctx->entries[i] = NULL;
        }
    }

    if (ctx->json_payload) {
        free(ctx->json_payload);
        ctx->json_payload = NULL;
    }

    ctx->count = 0;
    ctx->created_time = time(NULL);
    ctx->retry_count = 0;
    ctx->state = HTTP_IDLE;
}

/**
 * HTTP state machine implementation
 */
int collect_advance_http_state_machine(void) {
    time_t now = time(NULL);

    switch (current_batch.state) {
    case HTTP_IDLE:
        // Check if we should start a new batch
        if (current_batch.count >= (int)config_get_batch_size()) {
            console_debug(&csl, "Starting batch: reached max size (%d)", current_batch.count);
            current_batch.state = HTTP_PREPARING;
        } else if (current_batch.count > 0 &&
                   (now - current_batch.created_time) >= (config_get_batch_timeout_ms() / 1000)) {
            console_debug(&csl, "Starting batch: timeout reached (%d entries)", current_batch.count);
            current_batch.state = HTTP_PREPARING;
        }
        break;

    case HTTP_PREPARING:
        // Create JSON payload
        current_batch.json_payload =
            create_json_payload(current_batch.entries, current_batch.count, &current_batch.payload_size);

        if (current_batch.json_payload) {
            current_batch.state = HTTP_SENDING;
            console_debug(&csl, "Prepared batch with %d entries (%zu bytes)", current_batch.count,
                          current_batch.payload_size);
        } else {
            console_error(&csl, "Failed to create JSON payload");
            current_batch.state = HTTP_FAILED;
        }
        break;

    case HTTP_SENDING: {
        console_debug(&csl, "Starting HTTP request for batch with %d logs (%zu bytes)", 
                     current_batch.count, current_batch.payload_size);
        int result = send_http_request(current_batch.json_payload, current_batch.payload_size);

        if (result == 0) {
            console_info(&csl, "Successfully sent batch of %d logs", current_batch.count);
            clear_batch_context(&current_batch);
            last_batch_time = now;
            return 1; // Batch completed successfully
        } else {
            current_batch.retry_count++;
            if (current_batch.retry_count < (int)config_get_http_retries()) {
                console_warn(&csl, "HTTP send failed, retrying (%d/%u)", current_batch.retry_count,
                             config_get_http_retries());
                current_batch.state = HTTP_RETRY_WAIT;
            } else {
                console_error(&csl, "HTTP send failed after %u attempts", config_get_http_retries());
                current_batch.state = HTTP_FAILED;
            }
        }
    } break;

    case HTTP_RETRY_WAIT:
        // Simple delay before retry
        sleep(HTTP_RETRY_DELAY_MS / 1000);
        current_batch.state = HTTP_SENDING;
        break;

    case HTTP_FAILED:
        console_error(&csl, "Batch processing failed, dropping %d entries", current_batch.count);
        clear_batch_context(&current_batch);
        return -1; // Failed
    }

    return 0; // Continue processing
}

/**
 * Collect entries for a batch
 */
static void collect_entries_for_batch(void) {
    // Don't start new batch if current one is being processed
    if (current_batch.state != HTTP_IDLE) {
        return;
    }

    // Collect entries from queue
    while (current_batch.count < current_batch.max_count && queue.count > 0) {
        compact_log_entry_t *entry = dequeue_entry(&queue);
        if (entry) {
            current_batch.entries[current_batch.count] = entry;
            current_batch.count++;

            if (current_batch.count == 1) {
                current_batch.created_time = time(NULL);
            }
        }
    }
}

// Public API implementations

int collect_init(void) {
    console_info(&csl, "Initializing log collection system");

    // Load and validate configuration
    const collector_config_t *config = config_get_current();
    if (!config) {
        console_error(&csl, "Failed to load configuration");
        return -1;
    }

    if (config_validate(config) < 0) {
        console_error(&csl, "Configuration validation failed");
        return -1;
    }

    if (!config_is_enabled()) {
        console_warn(&csl, "Collector is disabled in configuration");
        return -1;
    }

    if (init_simple_queue(&queue) < 0) {
        console_error(&csl, "Failed to initialize queue");
        return -1;
    }

    if (init_entry_pool() < 0) {
        console_error(&csl, "Failed to initialize entry pool");
        return -1;
    }

    if (init_batch_context(&current_batch) < 0) {
        console_error(&csl, "Failed to initialize batch context");
        return -1;
    }

    dropped_count = 0;
    system_running = false;
    last_batch_time = time(NULL);

    if (init_http_client() < 0) {
        console_error(&csl, "Failed to initialize HTTP client");
        return -1;
    }

    system_running = true;

    console_info(&csl, "Single-core collection system initialized (max_queue_size=%u, max_batch_size=%u)",
                 config_get_queue_size(), config_get_batch_size());
    config_print_current();
    return 0;
}

int collect_process_pending_batches(void) {
    if (!system_running) {
        return -1;
    }

    // Collect entries for batching
    collect_entries_for_batch();

    // Advance state machine
    int result = collect_advance_http_state_machine();

    // Force processing if queue is getting full
    uint32_t urgent_threshold = config_get_queue_size() * 80 / 100;
    if (queue.count >= urgent_threshold && current_batch.state == HTTP_IDLE) {
        console_warn(&csl, "Queue urgent threshold reached, forcing batch processing");
        return collect_force_batch_processing();
    }

    return result;
}

void collect_cleanup(void) {
    console_info(&csl, "Cleaning up single-core collection system");

    system_running = false;

    // Process any remaining batch
    if (current_batch.count > 0) {
        console_info(&csl, "Processing final batch of %d entries", current_batch.count);
        current_batch.state = HTTP_PREPARING;
        while (current_batch.state != HTTP_IDLE && current_batch.state != HTTP_FAILED) {
            collect_advance_http_state_machine();
        }
    }

    // Clear batch context
    clear_batch_context(&current_batch);
    if (current_batch.entries) {
        free(current_batch.entries);
        current_batch.entries = NULL;
    }

    // Return remaining queue entries to pool
    while (queue.count > 0) {
        compact_log_entry_t *entry = dequeue_entry(&queue);
        if (entry) {
            collect_return_entry_to_pool(entry);
        }
    }

    // Free queue entries array
    if (queue.entries) {
        free(queue.entries);
        queue.entries = NULL;
    }

    // Free entry pool
    if (entry_pool) {
        free(entry_pool);
        entry_pool = NULL;
    }
    if (pool_used) {
        free(pool_used);
        pool_used = NULL;
    }

    cleanup_http_client();
    config_cleanup();

    console_info(&csl, "Single-core collection cleanup complete");
}

int collect_enqueue_log(const log_data_t *log_data) {
    if (!log_data || !log_data->msg || !system_running) {
        return -EINVAL;
    }

    // Check if we should accept logs (short circuit logic)
    if (!ubus_should_accept_logs()) {
        console_debug(&csl, "Rejecting log - log acceptance disabled");
        return -EPERM;
    }

    // Get entry from pool
    compact_log_entry_t *entry = collect_get_entry_from_pool();
    if (!entry) {
        dropped_count++;
        console_debug(&csl, "Entry pool exhausted, dropping log");
        return -ENOSPC;
    }

    // Store raw log fields without processing
    strncpy(entry->msg, log_data->msg, MAX_LOG_ENTRY_SIZE - 1);
    entry->msg[MAX_LOG_ENTRY_SIZE - 1] = '\0';
    
    entry->priority = log_data->priority;
    entry->source = log_data->source;
    entry->time = log_data->time;

    // Add to queue
    int result = enqueue_entry(&queue, entry);
    if (result < 0) {
        // Queue is full, drop the entry
        collect_return_entry_to_pool(entry);
        dropped_count++;
        console_debug(&csl, "Queue full, dropping log");
        return -ENOSPC;
    }

    return 0;
}

int collect_get_stats(uint32_t *queue_size, uint32_t *dropped_count_out) {
    if (!queue_size || !dropped_count_out) {
        return -EINVAL;
    }

    *queue_size = queue.count;
    *dropped_count_out = dropped_count;

    return 0;
}

bool collect_is_running(void) { return system_running; }

int collect_force_batch_processing(void) {
    if (!system_running) {
        return -1;
    }

    // Force current batch to be processed immediately
    if (current_batch.count > 0) {
        current_batch.state = HTTP_PREPARING;
        return collect_advance_http_state_machine();
    }

    return 0;
}

batch_context_t *collect_get_current_batch(void) { return &current_batch; }

/**
 * Report HTTP request failure for network monitoring
 */
void collect_report_http_failure(int error_code) {
    consecutive_http_failures++;
    console_warn(&csl, "HTTP failure reported (code: %d), consecutive failures: %d", error_code, consecutive_http_failures);
    
    // Report to UBUS module to potentially stop log acceptance
    ubus_report_network_failure(consecutive_http_failures);
}

/**
 * Report successful HTTP request to reset failure counter
 */
void collect_report_http_success(void) {
    if (consecutive_http_failures > 0) {
        console_info(&csl, "HTTP request successful, resetting failure counter (was: %d)", consecutive_http_failures);
        consecutive_http_failures = 0;
        
        // Report success to UBUS module
        ubus_report_network_failure(0);
    }
}

/**
 * Get current consecutive HTTP failure count
 */
int collect_get_consecutive_failures(void) {
    return consecutive_http_failures;
}
