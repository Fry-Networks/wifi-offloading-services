#include "services/access_token.h"
#include "core/console.h"
#include "core/uloop_scheduler.h"
#include "http/http-requests.h"
#include "services/config/config.h"
#include <json-c/json.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ACCESS_TOKEN_ENDPOINT "access"
#define ACCESS_TOKEN_FILE "access-token.json"
#define ACCESS_TOKEN_EXPIRATION_MARGIN 3600

static Console csl = {
    .topic = "access token",
};

bool save_access_token(char *access_token_json) {
    char access_token_file_path[256];
    snprintf(access_token_file_path, sizeof(access_token_file_path), "%s/%s", config.data_path, ACCESS_TOKEN_FILE);

    FILE *file = fopen(access_token_file_path, "w");
    if (file == NULL) {
        console_error(&csl, "failed to open access token file for writing");
        return false;
    }

    fprintf(file, "%s", access_token_json);
    fclose(file);

    return true;
}

char *read_access_token() {
    char access_token_file_path[256];
    snprintf(access_token_file_path, sizeof(access_token_file_path), "%s/%s", config.data_path, ACCESS_TOKEN_FILE);

    FILE *file = fopen(access_token_file_path, "r");
    if (file == NULL) {
        console_debug(&csl, "could not open access token file; it might not exist yet");
        return NULL;
    }

    // Move to the end of the file to determine its size
    if (fseek(file, 0, SEEK_END) != 0) {
        console_debug(&csl, "failed to seek to the end of the access token file");
        fclose(file);
        return NULL;
    }

    long file_size_long = ftell(file);
    if (file_size_long < 0) {
        console_debug(&csl, "failed to get file size of access token file");
        fclose(file);
        return NULL;
    }

    // Ensure the file fits in size_t and prevent integer overflow
    if ((unsigned long)file_size_long + 1 > SIZE_MAX) {
        console_debug(&csl, "access token file is too large");
        fclose(file);
        return NULL;
    }

    size_t file_size = (size_t)file_size_long;

    // Reset the file position to the beginning
    if (fseek(file, 0, SEEK_SET) != 0) {
        console_debug(&csl, "failed to seek to the beginning of the access token file");
        fclose(file);
        return NULL;
    }

    // Allocate memory for the access token
    char *access_token = malloc(file_size + 1);
    if (access_token == NULL) {
        console_debug(&csl, "failed to allocate memory for access token");
        fclose(file);
        return NULL;
    }

    // Read the file's contents
    size_t bytes_read = fread(access_token, 1, file_size, file);
    if (bytes_read != file_size) {
        console_error(&csl, "failed to read access token file");
        free(access_token);
        fclose(file);
        return NULL;
    }

    access_token[file_size] = '\0';

    fclose(file);
    return access_token;
}

bool parse_access_token(const char *access_token_json, AccessToken *access_token) {
    access_token->token = NULL;
    access_token->issued_at_seconds = 0;
    access_token->expires_at_seconds = 0;

    json_object *json = json_tokener_parse(access_token_json);
    if (json == NULL) {
        console_error(&csl, "failed to parse access token json");
        return access_token;
    }

    json_object *token_json = NULL;
    if (!json_object_object_get_ex(json, "token", &token_json)) {
        console_error(&csl, "failed to get token from access token json");
        json_object_put(json);
        return false;
    }

    const char *token_str = json_object_get_string(token_json);
    if (token_str == NULL) {
        console_error(&csl, "failed to get token string from access token json");
        json_object_put(json);
        return false;
    }

    access_token->token = strdup(token_str);
    if (access_token->token == NULL) {
        console_error(&csl, "failed to allocate memory for access token string");
        json_object_put(json);
        return false;
    }

    json_object *issued_at_json = NULL;
    if (!json_object_object_get_ex(json, "issued_at_seconds", &issued_at_json)) {
        console_error(&csl, "failed to get issued_at_seconds from access token json");
        free(access_token->token);
        access_token->token = NULL;
        json_object_put(json);
        return false;
    }

    if (!json_object_is_type(issued_at_json, json_type_int)) {
        console_error(&csl, "issued_at_seconds is not an integer");
        free(access_token->token);
        access_token->token = NULL;
        json_object_put(json);
        return false;
    }

    access_token->issued_at_seconds = json_object_get_int64(issued_at_json);

    json_object *expires_at_json = NULL;
    if (!json_object_object_get_ex(json, "expires_at_seconds", &expires_at_json)) {
        console_error(&csl, "failed to get expires_at_seconds from access token json");
        free(access_token->token);
        access_token->token = NULL;
        json_object_put(json);
        return false;
    }

    if (!json_object_is_type(expires_at_json, json_type_int)) {
        console_error(&csl, "expires_at_seconds is not an integer");
        free(access_token->token);
        access_token->token = NULL;
        json_object_put(json);
        return false;
    }

    access_token->expires_at_seconds = json_object_get_int64(expires_at_json);

    json_object_put(json);
    return true;
}

char *request_access_token(Registration *registration) {
    char url[256];
    snprintf(url, sizeof(url), "%s/%s", config.accounting_api, ACCESS_TOKEN_ENDPOINT);

    // Convert registration to json
    json_object *json_body = json_object_new_object();
    json_object_object_add(json_body, "fry_device_id", json_object_new_string(registration->fry_device_id));
    json_object_object_add(json_body, "access_key", json_object_new_string(registration->access_key));
    const char *body_json_str = json_object_to_json_string(json_body);
    console_debug(&csl, "access request body is %s", body_json_str);

    HttpPostOptions options = {
        .url = url,
        .body_json_str = body_json_str,
    };

    HttpResult result = http_post(&options);
    json_object_put(json_body);

    if (result.is_error) {
        console_error(&csl, "failed to request access token with error: %s", result.error);
        return NULL;
    }

    if (result.response_buffer == NULL) {
        console_error(&csl, "no access token data found in response");
        return NULL;
    }

    return result.response_buffer;
}

uint32_t calculate_next_delay_ms(time_t expires_at_seconds, time_t access_interval) {
    time_t now = time(NULL);
    time_t next_run = expires_at_seconds - ACCESS_TOKEN_EXPIRATION_MARGIN;

    // Check if the token has already expired or is about to expire
    if (next_run <= now) return 0; // Schedule immediately

    // Check if the next access interval is sooner than the expiration time
    time_t next_interval = now + access_interval;
    if (next_interval < next_run) {
        return (uint32_t)(access_interval * 1000); // Convert seconds to milliseconds
    }

    // Default to the expiration time
    time_t delay_seconds = next_run - now;
    return (uint32_t)(delay_seconds * 1000); // Convert seconds to milliseconds
}

AccessToken *init_access_token(Registration *registration) {
    AccessToken *access_token = (AccessToken *)malloc(sizeof(AccessToken));
    if (access_token == NULL) {
        console_error(&csl, "failed to allocate memory for access token");
        return NULL;
    }

    // Initialize access token (default values)
    access_token->token = NULL;
    access_token->issued_at_seconds = 0;
    access_token->expires_at_seconds = 0;

    // Try to read the access token and check if it's still valid
    char *saved_access_token = read_access_token();
    if (saved_access_token != NULL) {
        bool parse_result = parse_access_token(saved_access_token, access_token);
        free(saved_access_token);
        if (parse_result) {
            if (time(NULL) < access_token->expires_at_seconds - ACCESS_TOKEN_EXPIRATION_MARGIN) {
                return access_token;
            }
        }
    }

    // Request a new access token
    char *access_token_json_str = request_access_token(registration);
    if (access_token_json_str == NULL) {
        console_error(&csl, "failed to request access token");
        return access_token;
    }

    bool save_result = save_access_token(access_token_json_str);
    if (!save_result) {
        console_error(&csl, "failed to save access token");
        free(access_token_json_str);
        return access_token;
    }

    bool parse_result = parse_access_token(access_token_json_str, access_token);
    if (!parse_result) {
        console_error(&csl, "failed to parse access token");
        free(access_token_json_str);
        return access_token;
    }

    console_info(&csl, "access token initialized");
    free(access_token_json_str);
    return access_token;
}

void access_token_task(void *task_context) {
    console_debug(&csl, "access_token_task called with context: %p", task_context);

    if (task_context == NULL) {
        console_error(&csl, "access_token_task called with NULL context");
        return;
    }

    AccessTokenTaskContext *context = (AccessTokenTaskContext *)task_context;

    console_debug(&csl, "Context values - access_token: %p, registration: %p, callbacks: %p", context->access_token,
                  context->registration, context->callbacks);

    if (context->access_token == NULL || context->registration == NULL) {
        console_error(&csl, "Invalid context - access_token or registration is NULL");
        return;
    }

    console_debug(&csl, "Executing access token refresh task");

    char *access_token_json_str = request_access_token(context->registration);
    if (access_token_json_str == NULL) {
        console_error(&csl, "failed to request access token");
        // Still reschedule to retry later
        uint32_t retry_delay_ms = 60000; // Retry in 1 minute
        console_debug(&csl, "Scheduling retry in %u ms", retry_delay_ms);
        context->task_id = schedule_once(retry_delay_ms, access_token_task, context);
        return;
    }

    bool save_result = save_access_token(access_token_json_str);
    if (!save_result) {
        console_error(&csl, "failed to save access token");
        free(access_token_json_str);
        // Still reschedule to retry later
        uint32_t retry_delay_ms = 60000; // Retry in 1 minute
        context->task_id = schedule_once(retry_delay_ms, access_token_task, context);
        return;
    }

    bool parse_result = parse_access_token(access_token_json_str, context->access_token);
    if (!parse_result) {
        console_error(&csl, "failed to parse access token");
        free(access_token_json_str);
        // Still reschedule to retry later
        uint32_t retry_delay_ms = 60000; // Retry in 1 minute
        context->task_id = schedule_once(retry_delay_ms, access_token_task, context);
        return;
    }

    free(access_token_json_str);

    // Notify callback about token refresh
    if (context->callbacks && context->callbacks->on_token_refresh) {
        context->callbacks->on_token_refresh(context->access_token->token, context->callbacks->context);
    }

    // Schedule the next refresh
    uint32_t next_delay_ms = calculate_next_delay_ms(context->access_token->expires_at_seconds, config.access_interval);
    console_debug(&csl, "Scheduling next access token refresh in %u ms", next_delay_ms);
    context->task_id = schedule_once(next_delay_ms, access_token_task, context);
}

AccessTokenTaskContext *
access_token_service(AccessToken *access_token, Registration *registration, AccessTokenCallbacks *callbacks) {
    console_debug(&csl, "access_token_service called with access_token: %p, registration: %p, callbacks: %p",
                  access_token, registration, callbacks);

    if (access_token == NULL || registration == NULL) {
        console_error(&csl, "Invalid parameters - access_token or registration is NULL");
        return NULL;
    }

    AccessTokenTaskContext *context = (AccessTokenTaskContext *)malloc(sizeof(AccessTokenTaskContext));
    if (context == NULL) {
        console_error(&csl, "failed to allocate memory for access token task context");
        return NULL;
    }

    console_debug(&csl, "Allocated context at %p", context);

    context->access_token = access_token;
    context->registration = registration;
    context->callbacks = callbacks;
    context->task_id = 0; // Initialize task ID

    // Schedule the initial task
    uint32_t initial_delay_ms = calculate_next_delay_ms(access_token->expires_at_seconds, config.access_interval);
    console_info(&csl, "Starting access token service with initial delay of %u ms", initial_delay_ms);
    console_debug(&csl, "About to call schedule_once with delay %u ms, callback %p, context %p", initial_delay_ms,
                  (void *)access_token_task, context);

    context->task_id = schedule_once(initial_delay_ms, access_token_task, context);
    console_debug(&csl, "schedule_once returned task_id: %u", context->task_id);

    if (context->task_id == 0) {
        console_error(&csl, "failed to schedule access token task");
        free(context);
        return NULL;
    }

    console_debug(&csl, "Successfully scheduled access token task, returning context %p", context);
    return context;
}

void clean_access_token_context(AccessTokenTaskContext *context) {
    console_debug(&csl, "clean_access_token_context called with context: %p", context);
    if (context != NULL) {
        if (context->task_id != 0) {
            console_debug(&csl, "Cancelling access token task %u", context->task_id);
            cancel_task(context->task_id);
        }
        console_debug(&csl, "Freeing context %p", context);
        free(context);
    }
}

void clean_access_token(AccessToken *access_token) {
    if (access_token != NULL) {
        if (access_token->token != NULL) {
            free(access_token->token);
        }
        free(access_token);
    }
    console_info(&csl, "cleaned access token");
}

bool is_token_valid(AccessToken *access_token) {
    if (access_token == NULL || access_token->token == NULL) {
        console_error(&csl, "Invalid access token object or token is NULL");
        return false;
    }

    time_t current_time = time(NULL);
    if (current_time == ((time_t)-1)) {
        console_error(&csl, "Failed to get the current time");
        return false;
    }

    console_debug(&csl, "Current time: %ld, Expires at: %ld", current_time, access_token->expires_at_seconds);

    if (current_time >= access_token->expires_at_seconds) {
        console_debug(&csl, "Access token has expired");
        return false;
    }

    console_debug(&csl, "Access token is valid");
    return true;
}
