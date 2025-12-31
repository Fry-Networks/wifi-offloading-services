#include "device_status.h"
#include "core/console.h"
#include "core/uloop_scheduler.h"
#include "http/http-requests.h"
#include "services/access_token.h"
#include "services/config/config.h"
#include "services/device_info.h"
#include <json-c/json.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEVICE_STATUS_ENDPOINT "/api/nfnode/device-status"

static Console csl = {
    .topic = "device-status",
};

DeviceStatus device_status = Unknown;

bool on_boot = true;

DeviceStatus request_device_status(void *task_context) {
    DeviceStatusTaskContext *context = (DeviceStatusTaskContext *)task_context;
    // Url
    char device_status_url[256];
    snprintf(device_status_url, sizeof(device_status_url), "%s%s", config.main_api, DEVICE_STATUS_ENDPOINT);

    // Request body
    json_object *json_body = json_object_new_object();
    json_object_object_add(json_body, "on_boot", json_object_new_boolean(on_boot));
    // json_object_object_add(json_body, "device_id", json_object_new_string(context->device_info->device_id));
    if (strcmp(context->device_info->model, "Odyssey") != 0) {
        json_object_object_add(json_body, "device_id", json_object_new_string(context->device_info->device_id));
    }
    json_object_object_add(json_body, "mac", json_object_new_string(context->device_info->mac));
    json_object_object_add(json_body, "name", json_object_new_string(context->device_info->name));
    json_object_object_add(json_body, "brand", json_object_new_string(context->device_info->brand));
    json_object_object_add(json_body, "model", json_object_new_string(context->device_info->model));
    json_object_object_add(json_body, "public_ip", json_object_new_string(context->device_info->public_ip));
    json_object_object_add(json_body, "os_name", json_object_new_string(context->device_info->os_name));
    json_object_object_add(json_body, "os_version", json_object_new_string(context->device_info->os_version));
    json_object_object_add(json_body, "os_services_version",
                           json_object_new_string(context->device_info->os_services_version));
    json_object_object_add(json_body, "did_public_key", json_object_new_string(context->device_info->did_public_key));
    json_object_object_add(json_body, "fry_device_id", json_object_new_string(context->fry_device_id));
    const char *body = json_object_to_json_string(json_body);

    console_debug(&csl, "device status request body %s", body);

    HttpPostOptions options = {
        .url = device_status_url,
        .bearer_token = context->access_token->token,
        .body_json_str = body,
    };

    HttpResult result = http_post(&options);

    json_object_put(json_body);

    if (result.is_error) {
        console_error(&csl, "error requesting device status: %s", result.error);
        return Unknown;
    }

    if (result.response_buffer == NULL) {
        console_error(&csl, "no response received, assuming unknown status");
        return Unknown;
    }

    // Parse response
    struct json_object *parsed_response;
    struct json_object *device_status;

    parsed_response = json_tokener_parse(result.response_buffer);
    if (parsed_response == NULL) {
        // JSON parsing failed
        console_error(&csl, "failed to parse device status JSON data");
        free(result.response_buffer);
        return Unknown;
    }

    if (!json_object_object_get_ex(parsed_response, "deviceStatus", &device_status)) {
        console_error(&csl, "deviceStatus field missing or invalid");
        json_object_put(parsed_response);
        free(result.response_buffer);
        return Unknown;
    }

    int response_device_status = json_object_get_int64(device_status);

    json_object_put(parsed_response);
    free(result.response_buffer);

    console_debug(&csl, "device status response: %d", response_device_status);

    on_boot = false;

    return response_device_status;
}

void device_status_task(void *task_context) {
    DeviceStatusTaskContext *context = (DeviceStatusTaskContext *)task_context;
    device_status = request_device_status(context);
    console_debug(&csl, "device status: %d", device_status);
    console_debug(&csl, "device status interval: %d", config.device_status_interval);
    // No manual rescheduling needed - repeating tasks auto-reschedule
}

DeviceStatusTaskContext *
device_status_service(DeviceInfo *device_info, char *fry_device_id, AccessToken *access_token) {
    DeviceStatusTaskContext *context = (DeviceStatusTaskContext *)malloc(sizeof(DeviceStatusTaskContext));
    if (context == NULL) {
        console_error(&csl, "failed to allocate memory for device status task context");
        return NULL;
    }

    context->fry_device_id = fry_device_id;
    context->device_info = device_info;
    context->access_token = access_token;
    context->task_id = 0;

    // Convert seconds to milliseconds for scheduler
    uint32_t interval_ms = config.device_status_interval * 1000;
    uint32_t initial_delay_ms = config.device_status_interval * 1000; // Start after one interval

    console_info(&csl, "Starting device status service with interval %u ms", interval_ms);

    // Schedule repeating task
    context->task_id = schedule_repeating(initial_delay_ms, interval_ms, device_status_task, context);

    if (context->task_id == 0) {
        console_error(&csl, "failed to schedule device status task");
        free(context);
        return NULL;
    }

    console_debug(&csl, "Successfully scheduled device status task with ID %u", context->task_id);

    // Side effects
    // Make sure fry operator is running (all status codes but 6)
    // Start the peaq did service (on status 5)
    // Check that the captive portal is running (on status 6)
    // Disable fry operator network (on status 6)

    return context;
}

void clean_device_status_context(DeviceStatusTaskContext *context) {
    console_debug(&csl, "clean_device_status_context called with context: %p", context);
    if (context != NULL) {
        if (context->task_id != 0) {
            console_debug(&csl, "Cancelling device status task %u", context->task_id);
            cancel_task(context->task_id);
        }
        console_debug(&csl, "Freeing device status context %p", context);
        free(context);
    }
}
