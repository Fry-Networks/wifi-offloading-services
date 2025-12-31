#include "package_update.h"
#include "core/console.h"
#include "core/result.h"
#include "core/script_runner.h"
#include "core/uloop_scheduler.h"
#include "http/http-requests.h"
#include "services/access_token.h"
#include "services/config/config.h"
#include "services/device_info.h"
#include "services/registration.h"
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Console csl = {
    .topic = "package-update",
};

typedef struct {
    bool update_available;
    const char *download_link;
    const char *checksum;
    const char *size_bytes;
    const char *new_version;
} PackageCheckResult;

#define PACKAGE_STATUS_ENDPOINT "packages/status"
#define PACKAGE_CHECK_ENDPOINT "packages/check"
#define UPDATE_MARKER_FILE "/tmp/fry-os-services-update-marker"

void send_package_status(PackageUpdateTaskContext *ctx,
                         const char *status,
                         const char *error_message,
                         const char *new_version) {
    // Url
    char package_status_url[256];
    snprintf(package_status_url, sizeof(package_status_url), "%s/%s", config.devices_api, PACKAGE_STATUS_ENDPOINT);

    // Request body (note that error_message is optional)
    json_object *json_body = json_object_new_object();
    json_object_object_add(json_body, "package_name", json_object_new_string("fry-os-services"));
    json_object_object_add(json_body, "package_architecture", json_object_new_string(ctx->device_info->arch));
    json_object_object_add(json_body, "current_version", json_object_new_string(ctx->device_info->os_services_version));
    json_object_object_add(json_body, "package_status", json_object_new_string(status));
    json_object_object_add(json_body, "device_id", json_object_new_string(ctx->registration->fry_device_id));
    if (new_version != NULL) {
        json_object_object_add(json_body, "new_version", json_object_new_string(new_version));
    }
    if (error_message != NULL) {
        json_object_object_add(json_body, "error_message", json_object_new_string(error_message));
    }

    const char *body = json_object_to_json_string(json_body);

    console_debug(&csl, "package status request body: %s", body);

    // Send status to server
    HttpPostOptions options = {
        .url = package_status_url,
        .body_json_str = body,
        .bearer_token = ctx->access_token->token,
    };

    HttpResult result = http_post(&options);

    json_object_put(json_body);

    if (result.is_error) {
        console_error(&csl, "package status request failed: %s", result.error);

        // Try to parse response buffer to get error.message
        if (result.response_buffer != NULL) {
            struct json_object *json_parsed_error = json_tokener_parse(result.response_buffer);
            if (json_parsed_error != NULL) {
                struct json_object *json_error;
                if (json_object_object_get_ex(json_parsed_error, "error", &json_error)) {
                    struct json_object *json_message;
                    if (json_object_object_get_ex(json_error, "message", &json_message)) {
                        const char *error_message = json_object_get_string(json_message);
                        console_error(&csl, "error message from server: %s", error_message);
                    }
                }
                json_object_put(json_parsed_error);
            }
            free(result.response_buffer);
        }

        return;
    }

    if (result.response_buffer != NULL) {
        free(result.response_buffer);
    }
}

void check_package_update_completion(Registration *registration, DeviceInfo *device_info, AccessToken *access_token) {
    if (access(UPDATE_MARKER_FILE, F_OK) == 0) {
        // Read the file's contents
        char version[64];
        FILE *marker_file = fopen(UPDATE_MARKER_FILE, "r");
        if (marker_file != NULL) {
            fgets(version, sizeof(version), marker_file);
            fclose(marker_file);
        }

        // Compare the version with the current version
        if (strcmp(version, device_info->os_services_version) == 0) {
            console_info(&csl, "Package update completed successfully");
            PackageUpdateTaskContext ctx = {
                .device_info = device_info, .registration = registration, .access_token = access_token};
            send_package_status(&ctx, "completed", NULL, NULL);
        } else {
            console_error(&csl, "Package update failed");
        }

        remove(UPDATE_MARKER_FILE);
    } else {
        console_info(&csl, "No update marker found");
    }
}

void write_update_marker(const char *new_version) {
    FILE *marker_file = fopen(UPDATE_MARKER_FILE, "w");
    if (marker_file != NULL) {
        fprintf(marker_file, "%s", new_version);
        fclose(marker_file);
    }
}

void update_package(const char *file_path) {
    char command[256];
    snprintf(command, sizeof(command), "%s/%s %s", config.scripts_path, "run_opkg_upgrade.sh", file_path);
    char *output = run_script(command);
    if (output != NULL) {
        free(output);
    }
}

Result verify_package_checksum(const char *file_path, const char *checksum) {
    if (file_path == NULL || checksum == NULL) {
        return error(1, "Invalid parameters: file_path or checksum is NULL");
    }

    // Create command to calculate SHA256 checksum
    char command[256];
    snprintf(command, sizeof(command), "sha256sum '%s'", file_path);

    // Run the sha256sum command
    char *output = run_script(command);
    if (output == NULL) {
        return error(2, "Failed to run sha256sum command");
    }

    // Parse the output: sha256sum returns "<hash>  <filename>" (with 2 spaces)
    char calculated_checksum[65]; // SHA256 is 64 characters + null terminator
    sscanf(output, "%64s", calculated_checksum);

    // Free the output buffer
    free(output);

    // Compare the calculated checksum with the expected one
    if (strcmp(calculated_checksum, checksum) == 0) {
        console_debug(&csl, "Checksum verification successful");
        return ok(NULL);
    } else {
        console_error(&csl, "Checksum mismatch: expected %s, got %s", checksum, calculated_checksum);
        return error(3, "Checksum verification failed");
    }
}

/**
 * @brief Downloads a package from a given URL.
 *
 * @param ctx The context for the package update task.
 * @param download_link The URL to download the package from.
 * @param checksum The expected checksum of the package.
 *
 * @return Result struct containing:
 *         - On success (ok=true): A pointer to the downloaded package file path.
 *           The caller must free this pointer when done.
 *         - On failure (ok=false): Error details.
 */
Result download_package(PackageUpdateTaskContext *ctx, const char *download_link, const char *checksum) {
    if (ctx == NULL || download_link == NULL || checksum == NULL) {
        console_error(&csl, "Invalid parameters");
        return error(-1, "Invalid parameters");
    }

    // Prepare download path
    char download_path[256];
    snprintf(download_path, sizeof(download_path), "%s/%s", config.temp_path, "package-update.ipk");
    console_debug(&csl, "downloading package from: %s to %s", download_link, download_path);

    // Set up download options
    HttpDownloadOptions download_options = {
        .url = download_link,
        .download_path = download_path,
    };

    // Perform the download
    HttpResult download_result = http_download(&download_options);

    if (download_result.is_error) {
        console_error(&csl, "package download failed: %s", download_result.error);
        send_package_status(ctx, "error", "package download failed", NULL);
        return error(-1, "package download failed");
    }

    console_debug(&csl, "package downloaded successfully");
    return ok(strdup(download_path));
}

/**
 * @brief Sends a package check request to the backend to determine if an update is available.
 * If an update is available, it returns the update details in a PackageCheckResult structure.
 *
 * @param ctx Pointer to the PackageUpdateTaskContext struct.
 *
 * @return Result struct containing:
 *         - On success (ok=true): A pointer to a PackageCheckResult structure.
 *           The caller must free this structure and its string fields
 *           (download_link, checksum, size_bytes, new_version) when done.
 *         - On failure (ok=false): Error details.
 */
Result send_package_check_request(PackageUpdateTaskContext *ctx) {
    // Url
    char package_update_url[256];
    snprintf(package_update_url, sizeof(package_update_url), "%s/%s", config.devices_api, PACKAGE_CHECK_ENDPOINT);
    console_debug(&csl, "package update url: %s", package_update_url);

    // Request body
    json_object *json_body = json_object_new_object();
    json_object_object_add(json_body, "package_name", json_object_new_string("fry-os-services"));
    json_object_object_add(json_body, "package_architecture", json_object_new_string(ctx->device_info->arch));
    json_object_object_add(json_body, "current_version", json_object_new_string(ctx->device_info->os_services_version));
    json_object_object_add(json_body, "device_id", json_object_new_string(ctx->registration->fry_device_id));
    const char *body = json_object_to_json_string(json_body);

    console_debug(&csl, "package check request body: %s", body);

    HttpPostOptions options = {
        .url = package_update_url,
        .body_json_str = body,
        .bearer_token = ctx->access_token->token,
    };

    HttpResult result = http_post(&options);

    json_object_put(json_body);

    if (result.is_error) {
        console_error(&csl, "package update request failed: %s", result.error);

        // Try to parse response buffer to get error.message
        if (result.response_buffer != NULL) {
            struct json_object *json_parsed_error = json_tokener_parse(result.response_buffer);
            if (json_parsed_error != NULL) {
                struct json_object *json_error;
                if (json_object_object_get_ex(json_parsed_error, "error", &json_error)) {
                    struct json_object *json_message;
                    if (json_object_object_get_ex(json_error, "message", &json_message)) {
                        const char *error_message = json_object_get_string(json_message);
                        console_error(&csl, "error message from server: %s", error_message);
                    }
                }
                json_object_put(json_parsed_error);
            }
            free(result.response_buffer);
        }

        return error(-1, "package update request failed");
    }

    if (result.response_buffer == NULL) {
        console_error(&csl, "package update request failed: response buffer is NULL");
        return error(-1, "response buffer is null");
    }

    // Parse server response
    struct json_object *json_parsed_response;
    struct json_object *json_data;
    struct json_object *json_update_available;
    struct json_object *json_download_link;
    struct json_object *json_checksum;
    struct json_object *json_size_bytes;
    struct json_object *json_new_version;

    json_parsed_response = json_tokener_parse(result.response_buffer);
    if (json_parsed_response == NULL) {
        console_error(&csl, "failed to parse package update JSON response");
        free(result.response_buffer);
        return error(-1, "failed to parse package update JSON response");
    }

    // Get the data object, which contains all other fields
    if (!json_object_object_get_ex(json_parsed_response, "data", &json_data)) {
        console_error(&csl, "missing 'data' field in package update response");
        json_object_put(json_parsed_response);
        free(result.response_buffer);
        return error(-1, "missing 'data' field in package update response");
    }

    // Extract fields from the data object
    if (!json_object_object_get_ex(json_data, "update_available", &json_update_available)) {
        console_error(&csl, "missing 'update_available' field in package update response");
        json_object_put(json_parsed_response);
        free(result.response_buffer);
        return error(-1, "missing 'update_available' field in package update response");
    }

    bool update_available = json_object_get_boolean(json_update_available);
    if (!update_available) {
        console_debug(&csl, "no update available");
        json_object_put(json_parsed_response);
        free(result.response_buffer);
        PackageCheckResult check_result = {false, NULL, NULL, NULL, NULL};
        return ok(&check_result);
    }

    bool error_extracting = false;
    if (!json_object_object_get_ex(json_data, "download_link", &json_download_link)) {
        console_error(&csl, "missing 'download_link' field in package update response");
        error_extracting = true;
    }
    if (!json_object_object_get_ex(json_data, "checksum", &json_checksum)) {
        console_error(&csl, "missing 'checksum' field in package update response");
        error_extracting = true;
    }
    if (!json_object_object_get_ex(json_data, "size_bytes", &json_size_bytes)) {
        console_error(&csl, "missing 'size_bytes' field in package update response");
        error_extracting = true;
    }
    if (!json_object_object_get_ex(json_data, "new_version", &json_new_version)) {
        console_error(&csl, "missing 'new_version' field in package update response");
        error_extracting = true;
    }

    if (error_extracting) {
        console_error(&csl, "error extracting fields from package update response");
        json_object_put(json_parsed_response);
        free(result.response_buffer);
        return error(-1, "error extracting fields from package update response");
    }

    const char *download_link = json_object_get_string(json_download_link);
    const char *checksum = json_object_get_string(json_checksum);
    const char *size_bytes = json_object_get_string(json_size_bytes);
    const char *new_version = json_object_get_string(json_new_version);

    console_debug(&csl, "download link: %s", download_link);
    console_debug(&csl, "checksum: %s", checksum);
    console_debug(&csl, "size bytes: %s", size_bytes);
    console_debug(&csl, "new version: %s", new_version);

    PackageCheckResult *check_result = malloc(sizeof(PackageCheckResult));
    if (!check_result) {
        return error(-1, "failed to allocate memory for PackageCheckResult");
    }

    check_result->update_available = true;
    check_result->download_link = strdup(download_link);
    check_result->checksum = strdup(checksum);
    check_result->size_bytes = strdup(size_bytes);
    check_result->new_version = strdup(new_version);

    json_object_put(json_parsed_response);
    free(result.response_buffer);

    return ok(check_result);
}

/**
 * @brief Handles the entire package update process. It queries the server for available updates.
 * If an update is found, it downloads the package, verifies it, and installs it.
 *
 * If errors occur during the update process, the function will:
 * - Report these errors to the backend.
 * - Reschedule itself for later execution.
 *
 * @param sch Pointer to the scheduler that manages this task.
 * @param task_context Pointer to the PackageUpdateTaskContext struct.
 *
 * @return void. Function either completes the update process or reschedules itself after reporting any errors.
 */
void package_update_task(void *task_context) {
    PackageUpdateTaskContext *ctx = (PackageUpdateTaskContext *)task_context;

    if (config.package_update_enabled == 0) {
        console_debug(&csl, "package update is disabled by configuration; will not reschedule package update task");
        return;
    }

    console_debug(&csl, "package update task");

    // Check if an update is available
    Result result = send_package_check_request(ctx);
    if (!result.ok) {
        console_error(&csl, result.error.message);
        return;
    }

    // Make sure result is valid, and that an update is available
    PackageCheckResult *package_check_result = (PackageCheckResult *)result.data;
    if (!package_check_result) {
        console_error(&csl, "package check result is NULL");
        return;
    }

    if (!package_check_result->update_available) {
        console_debug(&csl, "no package update available");
        return;
    }

    send_package_status(ctx, "in_progress", NULL, package_check_result->new_version);

    // Download the package
    Result download_result = download_package(ctx, package_check_result->download_link, package_check_result->checksum);
    if (!download_result.ok) {
        send_package_status(ctx, "error", download_result.error.message, NULL);
        return;
    }

    // Verify the package's checksum
    const char *download_path = download_result.data;
    Result verify_result = verify_package_checksum(download_path, package_check_result->checksum);
    if (!verify_result.ok) {
        send_package_status(ctx, "error", verify_result.error.message, NULL);
        return;
    }

    // Write the update marker
    write_update_marker(package_check_result->new_version);

    // Proceed with update
    update_package(download_path);

    // Free allocated data
    free((void *)download_path);
    if (package_check_result != NULL) {
        if (package_check_result->download_link != NULL) {
            free((void *)package_check_result->download_link);
        }
        if (package_check_result->checksum != NULL) {
            free((void *)package_check_result->checksum);
        }
        if (package_check_result->size_bytes != NULL) {
            free((void *)package_check_result->size_bytes);
        }
        if (package_check_result->new_version != NULL) {
            free((void *)package_check_result->new_version);
        }
        free((void *)package_check_result);
    }
}

/**
 * @brief Sets up the package update service by creating a scheduler-compatible task context.
 * It then runs the first execution of the package update task.
 *
 * @param sch Pointer to the scheduler that will manage the update task.
 * @param device_info Pointer to the device information structure.
 * @param registration Pointer to the registration information structure.
 * @param access_token Pointer to the access token structure for API authentication.
 *
 * @return void. If memory allocation for the context fails, an error is logged and the function returns without
 * scheduling.
 */
PackageUpdateTaskContext *
package_update_service(DeviceInfo *device_info, Registration *registration, AccessToken *access_token) {
    PackageUpdateTaskContext *ctx = (PackageUpdateTaskContext *)malloc(sizeof(PackageUpdateTaskContext));
    if (ctx == NULL) {
        console_error(&csl, "failed to allocate memory for package update task context");
        return NULL;
    }

    ctx->device_info = device_info;
    ctx->registration = registration;
    ctx->access_token = access_token;
    ctx->task_id = 0;

    // Convert seconds to milliseconds for scheduler
    uint32_t interval_ms = config.package_update_interval * 1000;
    uint32_t initial_delay_ms = config.package_update_interval * 1000; // Start after one interval

    console_info(&csl, "Starting package update service with interval %u ms", interval_ms);

    // Schedule repeating task
    ctx->task_id = schedule_repeating(initial_delay_ms, interval_ms, package_update_task, ctx);

    if (ctx->task_id == 0) {
        console_error(&csl, "failed to schedule package update task");
        free(ctx);
        return NULL;
    }

    console_debug(&csl, "Successfully scheduled package update task with ID %u", ctx->task_id);
    return ctx;
}

void clean_package_update_context(PackageUpdateTaskContext *context) {
    console_debug(&csl, "clean_package_update_context called with context: %p", context);
    if (context != NULL) {
        if (context->task_id != 0) {
            console_debug(&csl, "Cancelling package update task %u", context->task_id);
            cancel_task(context->task_id);
        }
        console_debug(&csl, "Freeing package update context %p", context);
        free(context);
    }
}
