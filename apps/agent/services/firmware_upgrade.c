#include "firmware_upgrade.h"
#include "core/console.h"
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
#include <sys/stat.h>
#include <unistd.h>

#define FIRMWARE_ENDPOINT "/firmware-updates/check-updates"
#define START_UPGRADE_ENDPOINT "/firmware-updates/start"
#define REPORT_STATUS_ENDPOINT "/firmware-updates/report-status"
#define VERIFY_STATUS_ENDPOINT "/firmware-updates/on-boot"
#define REQUEST_BODY_BUFFER_SIZE 256

static Console csl = {
    .topic = "firmware-upgrade",
};

int run_sysupgrade() {

    char script_path[256];
    char image_path[256];
    char option[4];

    snprintf(script_path, sizeof(script_path), "%s/run_sysupgrade.sh", config.scripts_path);
    snprintf(image_path, sizeof(image_path), "%s", config.temp_path);
    snprintf(option, sizeof(option), config.use_n_sysupgrade ? "-n" : "");

    char command[256];
    snprintf(command, sizeof(command), "%s %s %s", script_path, image_path, option);

    if (config.use_n_sysupgrade) {
        console_debug(&csl, "running sysupgrade script: %s (with -n)", command);
    } else {
        console_debug(&csl, "running sysupgrade script: %s (without -n)", command);
    }

    char *script_output = run_script(command);

    if (script_output) {
        console_debug(&csl, "sysupgrade script output: %s", script_output);
        int result = (*script_output == '1') ? 1 : -1;
        free(script_output);
        return result;
    }

    return -1;
}

void report_upgrade_status(AccessToken *access_token, int upgrade_attempt_id, const char *upgrade_status) {
    char report_status_url[256];
    snprintf(report_status_url, sizeof(report_status_url), "%s%s", config.accounting_api, REPORT_STATUS_ENDPOINT);
    // snprintf(report_status_url, sizeof(report_status_url), "%s%s", "http://localhost:4050", REPORT_STATUS_ENDPOINT);

    json_object *json_body = json_object_new_object();
    json_object_object_add(json_body, "upgrade_attempt_id", json_object_new_int(upgrade_attempt_id));
    json_object_object_add(json_body, "upgrade_status", json_object_new_string(upgrade_status));
    const char *body = json_object_to_json_string(json_body);

    console_debug(&csl, "reporting upgrade status with request body: %s", body);

    HttpPostOptions options = {
        .url = report_status_url,
        .body_json_str = body,
        .bearer_token = access_token->token,
    };

    HttpResult result = http_post(&options);

    json_object_put(json_body);

    if (result.is_error) {
        console_error(&csl, "failed to report upgrade status");
        console_error(&csl, "error: %s", result.error);
        return;
    }

    if (result.response_buffer == NULL) {
        console_error(&csl, "failed to report upgrade status");
        console_error(&csl, "no response received");
        return;
    }

    console_debug(&csl, "reported upgrade status successfully");
    free(result.response_buffer);
}

int execute_firmware_verification() {
    char script_path[256];
    char image_path[256];

    snprintf(script_path, sizeof(script_path), "%s/verify_firmware.sh", config.scripts_path);
    snprintf(image_path, sizeof(image_path), "%s", config.temp_path);

    char command[256];
    snprintf(command, sizeof(command), "%s %s", script_path, image_path);
    console_debug(&csl, "running command: %s", command);

    char *script_output = run_script(command);

    if (script_output) {
        console_debug(&csl, "script output: %s", script_output);
        int result = (*script_output == '1') ? 1 : -1;
        free(script_output);
        return result;
    }

    return -1;
}

void parse_outputf(const char *output, size_t *memory_free) {
    const char *key = "memory_free:";
    char *start = strstr(output, key);
    if (start) {
        start += strlen(key);
        *memory_free = strtoull(start, NULL, 10);
    }
}

// Check available memory
bool check_memory_and_proceed() {

    char sysupgrade_path[256];
    snprintf(sysupgrade_path, sizeof(sysupgrade_path), "%s/firmware.bin", config.temp_path);

    struct stat st;
    if (stat(sysupgrade_path, &st) != 0) {
        console_error(&csl, "failed to get image size for %s", sysupgrade_path);
        // report_upgrade_status(access_token, upgrade_attempt_id, "image_size_error");
        return false;
    }

    // Get image size
    size_t image_size = (size_t)st.st_size;
    console_debug(&csl, "image size: %zu bytes", image_size);

    // Run Lua script to get free memory
    char script_file[256];
    snprintf(script_file, sizeof(script_file), "%s%s", config.scripts_path, "/retrieve-data.lua");
    char *output = run_script(script_file);
    if (output == NULL) {
        console_error(&csl, "failed to run script %s", script_file);
        // report_upgrade_status(access_token, upgrade_attempt_id, "memory_check_failed");
        return false;
    }

    size_t memory_free = 0;
    parse_outputf(output, &memory_free);

    if (memory_free == 0) {
        console_error(&csl, "failed to parse memory_free from script output");
        free(output);
        return false;
    }

    console_info(&csl, "free memory: %zu bytes", memory_free);
    free(output);

    // Compare free memory to image size
    if (image_size > memory_free) {
        console_error(&csl, "insufficient memory. required: %zu bytes, available: %zu bytes", image_size, memory_free);
        console_info(&csl, "insufficient memory. not proceeding with the upgrade.");
        return false;
    }

    console_info(&csl, "sufficient memory. proceeding with the upgrade.");
    return true;
}

int run_firmware_test() {

    char script_path[256];
    char image_path[256];

    snprintf(script_path, sizeof(script_path), "%s/run_sysupgrade_test.sh", config.scripts_path);
    snprintf(image_path, sizeof(image_path), "%s", config.temp_path);

    char command[256];
    snprintf(command, sizeof(command), "%s %s", script_path, image_path);

    char *script_output = run_script(command);

    if (script_output) {
        console_debug(&csl, "sysupgrade test script output: %s", script_output);
        int result = (*script_output == '1') ? 1 : -1;
        free(script_output);
        return result;
    }

    return -1;
}

void handle_download_result(AccessToken *access_token, int upgrade_attempt_id, bool success) {
    if (success) {
        report_upgrade_status(access_token, upgrade_attempt_id, "download_confirmed");

        int script_result = execute_firmware_verification();

        if (script_result == 1) {
            // Verification successful
            console_info(&csl, "the image is correct, the hashes match");
            report_upgrade_status(access_token, upgrade_attempt_id, "hash_verification_confirmed");

            bool check_memory = check_memory_and_proceed();

            if (check_memory) {
                report_upgrade_status(access_token, upgrade_attempt_id, "sufficient_memory");

                int firmware_test = run_firmware_test();

                if (firmware_test == 1) {

                    console_info(&csl, "firmware test successful, proceeding with upgrade");

                    report_upgrade_status(access_token, upgrade_attempt_id, "test_successfull");

                    int upgrade_result = run_sysupgrade();

                    if (upgrade_result == -1) {
                        report_upgrade_status(access_token, upgrade_attempt_id, "sysupgrade_failed");
                        // schedule_task(NULL, time(NULL) + 3600, firmware_upgrade_task, "sysupgrade_retry");
                        // Reschedule task

                        /*if (upgrade_result == 1) {
                            report_upgrade_status(upgrade_attempt_id, "upgrading");
                        } else {
                            report_upgrade_status(upgrade_attempt_id, "sysupgrade_failed");
                            // Reschedule task
                            // schedule_task(NULL, time(NULL) + 3600, firmware_upgrade_task, "sysupgrade_retry");
                        }*/
                    }
                } else {
                    console_info(&csl, "firmware test failed, upgrade does not continue");

                    report_upgrade_status(access_token, upgrade_attempt_id, "test_failed");
                }

            } else {

                report_upgrade_status(access_token, upgrade_attempt_id, "insufficient_memory");
            }

        } else {
            // Verification failed
            console_info(&csl, "the image is incorrect, the hashes do not match");
            report_upgrade_status(access_token, upgrade_attempt_id, "hash_verification_failed");
            // Reschedule verification in an hour
            // schedule_task(NULL, time(NULL) + 3600, firmware_upgrade_task, "hash_verification_retry");
        }

    } else {
        report_upgrade_status(access_token, upgrade_attempt_id, "download_failed");
        // Schedule a retry in one hour
        // schedule_task(sch, time(NULL) + 3600, firmware_upgrade_task, "firmware_upgrade_retry");
    }
}

void send_firmware_check_request(const char *codename,
                                 const char *version,
                                 const char *fry_device_id,
                                 AccessToken *access_token) {

    if (config.firmware_update_enabled == 0) {
        console_debug(&csl, "firmware update is disabled by configuration; will not proceed");
        return;
    }

    // Url
    char firmware_upgrade_url[256];
    snprintf(firmware_upgrade_url, sizeof(firmware_upgrade_url), "%s%s", config.accounting_api, FIRMWARE_ENDPOINT);
    // snprintf(firmware_upgrade_url, sizeof(firmware_upgrade_url), "%s%s", "http://localhost:4050", FIRMWARE_ENDPOINT);

    console_debug(&csl, "firmware endpoint: %s", firmware_upgrade_url);

    // Request body
    json_object *json_body = json_object_new_object();
    json_object_object_add(json_body, "codename", json_object_new_string(codename));
    json_object_object_add(json_body, "version", json_object_new_string(version));
    json_object_object_add(json_body, "fry_device_id", json_object_new_string(fry_device_id));
    const char *body = json_object_to_json_string(json_body);

    console_debug(&csl, "check firmware update body: %s", body);

    HttpPostOptions options = {
        .url = firmware_upgrade_url,
        .body_json_str = body,
        .bearer_token = access_token->token,
    };

    HttpResult result = http_post(&options);

    json_object_put(json_body);

    if (result.is_error) {
        console_error(&csl, "failed to check firmware update");
        console_error(&csl, "error: %s", result.error);
        return;
    }

    if (result.response_buffer == NULL) {
        console_error(&csl, "no response received");
        console_error(&csl, "failed to check firmware update");
        return;
    }

    // Parse server response
    struct json_object *parsed_response;
    struct json_object *updateAvailable;
    struct json_object *url = NULL;
    struct json_object *latestVersion;
    struct json_object *id = NULL;

    parsed_response = json_tokener_parse(result.response_buffer);
    if (parsed_response == NULL) {
        // JSON parsing failed
        console_error(&csl, "failed to parse firmware update JSON data");
        free(result.response_buffer);
        return;
    }

    // Extract fields
    bool error_occurred = false;
    if (!json_object_object_get_ex(parsed_response, "updateAvailable", &updateAvailable)) {
        console_warn(&csl, "updateAvailable field missing or invalid");
        error_occurred = true;
    }

    if (!json_object_object_get_ex(parsed_response, "url", &url)) {
        console_warn(&csl, "url field missing or invalid");
        url = NULL;
    }

    if (!json_object_object_get_ex(parsed_response, "latestVersion", &latestVersion)) {
        console_warn(&csl, "latestVersion field missing or invalid");
        error_occurred = true;
    }

    if (!json_object_object_get_ex(parsed_response, "id", &id)) {
        console_warn(&csl, "id field missing or invalid, setting to default");
        id = NULL;
    }

    if (error_occurred) {
        console_error(&csl, "error processing firmware update response");
        json_object_put(parsed_response);
        free(result.response_buffer);
        return;
    }

    int update_available = json_object_get_int(updateAvailable);
    const char *update_url = (url != NULL) ? json_object_get_string(url) : NULL;
    const char *latest_version = json_object_get_string(latestVersion);
    int upgrade_attempt_id = (id != NULL) ? json_object_get_int(id) : -1;

    console_debug(&csl, "update available: %d", update_available);
    console_debug(&csl, "update message or URL: %s", update_url);
    console_debug(&csl, "target firmware version: %s", latest_version);

    if (update_available == 2) {

        console_debug(&csl, "starting firmware download from: %s", update_url);

        char download_path[256];
        snprintf(download_path, sizeof(download_path), "%s/firmware.tar.gz", config.temp_path);

        // Download firmware
        HttpDownloadOptions download_options = {
            .url = update_url,
            .download_path = download_path,
        };

        HttpResult download_result = http_download(&download_options);
        handle_download_result(access_token, upgrade_attempt_id, !download_result.is_error);

    } else if (update_available == 1) {
        console_debug(&csl, "new version available: %s. update pending", latest_version);
        // Retask
    } else if (update_available == 0) {
        console_info(&csl, "no updates available");
    } else {
        console_error(&csl, "Unknown updateAvailable value received: %d", update_available);
    }

    json_object_put(parsed_response);
    free(result.response_buffer);
}

void firmware_upgrade_task(void *task_context) {
    FirmwareUpgradeTaskContext *context = (FirmwareUpgradeTaskContext *)task_context;
    console_debug(&csl, "firmware upgrade task");
    send_firmware_check_request(context->device_info->name, context->device_info->os_version,
                                context->registration->fry_device_id, context->access_token);
    // No manual rescheduling needed - repeating tasks auto-reschedule
}

FirmwareUpgradeTaskContext *
firmware_upgrade_check(DeviceInfo *device_info, Registration *registration, AccessToken *access_token) {
    FirmwareUpgradeTaskContext *context = (FirmwareUpgradeTaskContext *)malloc(sizeof(FirmwareUpgradeTaskContext));
    if (context == NULL) {
        console_error(&csl, "failed to allocate memory for firmware upgrade task context");
        return NULL;
    }

    context->device_info = device_info;
    context->registration = registration;
    context->access_token = access_token;
    context->task_id = 0;

    // Convert seconds to milliseconds for scheduler
    uint32_t interval_ms = config.firmware_update_interval * 1000;
    uint32_t initial_delay_ms = config.firmware_update_interval * 1000; // Start after one interval

    console_info(&csl, "Starting firmware upgrade service with interval %u ms", interval_ms);

    // Schedule repeating task
    context->task_id = schedule_repeating(initial_delay_ms, interval_ms, firmware_upgrade_task, context);

    if (context->task_id == 0) {
        console_error(&csl, "failed to schedule firmware upgrade task");
        free(context);
        return NULL;
    }

    console_debug(&csl, "Successfully scheduled firmware upgrade task with ID %u", context->task_id);
    return context;
}

void clean_firmware_upgrade_context(FirmwareUpgradeTaskContext *context) {
    console_debug(&csl, "clean_firmware_upgrade_context called with context: %p", context);
    if (context != NULL) {
        if (context->task_id != 0) {
            console_debug(&csl, "Cancelling firmware upgrade task %u", context->task_id);
            cancel_task(context->task_id);
        }
        console_debug(&csl, "Freeing firmware upgrade context %p", context);
        free(context);
    }
}

void clean_firmware_upgrade_service() {
    // Clean up if necessary
}

void firmware_upgrade_on_boot(Registration *registration, DeviceInfo *device_info, AccessToken *access_token) {

    if (config.firmware_update_enabled == 0) {
        console_debug(&csl, "firmware upgrade on boot is disabled by configuration; will not proceed.");
        return;
    }

    console_debug(&csl, "starting firmware_upgrade_on_boot");
    char verify_status_url[256];
    snprintf(verify_status_url, sizeof(verify_status_url), "%s%s", config.accounting_api, VERIFY_STATUS_ENDPOINT);
    // snprintf(verify_status_url, sizeof(verify_status_url), "%s%s", "http://localhost:4050", VERIFY_STATUS_ENDPOINT);

    if (registration == NULL || registration->fry_device_id == NULL) {
        console_error(&csl, "registration or fry_device_id is NULL");
        return;
    }

    if (device_info == NULL || device_info->os_version == NULL) {
        console_error(&csl, "device_info or os_version is NULL");
        return;
    }

    json_object *json_body = json_object_new_object();
    json_object_object_add(json_body, "fry_device_id", json_object_new_string(registration->fry_device_id));
    json_object_object_add(json_body, "os_version", json_object_new_string(device_info->os_version));
    const char *body = json_object_to_json_string(json_body);

    console_debug(&csl, "verifying firmware status on boot with request body: %s", body);

    HttpPostOptions options = {
        .url = verify_status_url,
        .body_json_str = body,
        .bearer_token = access_token->token,
    };

    HttpResult result = http_post(&options);
    console_debug(&csl, "HTTP request completed");

    json_object_put(json_body);

    if (result.is_error) {
        console_error(&csl, "failed to verify firmware status on boot");
        console_error(&csl, "error: %s", result.error);

        return;
    }

    if (result.response_buffer == NULL) {
        console_error(&csl, "failed to verify firmware status on boot");
        console_error(&csl, "no response received");

        return;
    }

    // Process the server's response
    struct json_object *parsed_response;
    struct json_object *status;

    parsed_response = json_tokener_parse(result.response_buffer);
    if (parsed_response == NULL) {
        console_error(&csl, "failed to parse verification response JSON data");
        free(result.response_buffer);
        return;
    }

    if (!json_object_object_get_ex(parsed_response, "status", &status)) {
        console_error(&csl, "status field missing or invalid");
        json_object_put(parsed_response);
        free(result.response_buffer);
        return;
    }

    const char *status_value = json_object_get_string(status);
    console_debug(&csl, "firmware status on boot: %s", status_value);
    console_info(&csl, "firmware status on boot complete");

    json_object_put(parsed_response);
    free(result.response_buffer);
}
