#include "device-context.h"
#include "core/console.h"
#include "core/uloop_scheduler.h"
#include "http/http-requests.h"
#include "services/access_token.h"
#include "services/config/config.h"
#include "services/registration.h"
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEVICE_ENDPOINT "devices"
#define DEVICE_CONTEXT_ENDPOINT "context"

static Console csl = {
    .topic = "device-context",
};

char *request_device_context(Registration *registration, AccessToken *access_token) {
    char url[256];
    snprintf(url, sizeof(url), "%s/%s/%s/%s", config.accounting_api, DEVICE_ENDPOINT, registration->fry_device_id,
             DEVICE_CONTEXT_ENDPOINT);

    // print token
    console_debug(&csl, "url: %s", url);
    console_debug(&csl, "access token: %s", access_token->token);

    HttpGetOptions options = {.url = url, .bearer_token = access_token->token};

    HttpResult result = http_get(&options);
    if (result.is_error) {
        console_error(&csl, "failed to request device context");
        console_error(&csl, "error: %s", result.error);
        return NULL;
    }

    if (result.response_buffer == NULL) {
        console_error(&csl, "no response received");
        return NULL;
    }

    return result.response_buffer;
}

void parse_and_update_device_context(DeviceContext *device_context, char *device_context_json) {
    json_object *json = json_tokener_parse(device_context_json);
    if (json == NULL) {
        console_error(&csl, "failed to parse device context json");
        return;
    }

    json_object *site_json = NULL;
    if (!json_object_object_get_ex(json, "site", &site_json)) {
        console_debug(&csl, "failed to get site from device context json");
        json_object_put(json);
        return;
    }

    json_object *site_id_json = NULL;
    if (!json_object_object_get_ex(site_json, "id", &site_id_json)) {
        console_debug(&csl, "failed to get site id from device context json; device might not be part of a site");
        json_object_put(json);
        return;
    }

    json_object *site_name_json = NULL;
    if (!json_object_object_get_ex(site_json, "name", &site_name_json)) {
        console_debug(&csl, "failed to get site name from device context json");
        json_object_put(json);
        return;
    }

    json_object *site_mac_json = NULL;
    if (!json_object_object_get_ex(site_json, "mac", &site_mac_json)) {
        console_debug(&csl, "failed to get site mac from device context json");
        json_object_put(json);
        return;
    }

    device_context->site->id = strdup(json_object_get_string(site_id_json));
    device_context->site->name = strdup(json_object_get_string(site_name_json));
    device_context->site->mac = strdup(json_object_get_string(site_mac_json));

    json_object_put(json);
    free(device_context_json);
}

DeviceContext *init_device_context(Registration *registration, AccessToken *access_token) {
    DeviceContext *device_context = (DeviceContext *)malloc(sizeof(DeviceContext));
    Site *site = (Site *)malloc(sizeof(Site));
    device_context->site = site;
    device_context->site->id = NULL;
    device_context->site->name = NULL;
    device_context->site->mac = NULL;

    char *device_context_json = request_device_context(registration, access_token);
    if (device_context_json == NULL) {
        console_debug(&csl, "failed to request device context");
        return device_context;
    }

    parse_and_update_device_context(device_context, device_context_json);
    console_info(&csl, "device context initialized");
    return device_context;
}

void device_context_task(void *task_context) {
    DeviceContextTaskContext *context = (DeviceContextTaskContext *)task_context;

    char *device_context_json = request_device_context(context->registration, context->access_token);
    if (device_context_json == NULL) {
        console_debug(&csl, "failed to request device context");
        return;
    }

    parse_and_update_device_context(context->device_context, device_context_json);
    console_info(&csl, "device context checked");
    // No manual rescheduling needed - repeating tasks auto-reschedule
}

DeviceContextTaskContext *
device_context_service(DeviceContext *device_context, Registration *registration, AccessToken *access_token) {
    DeviceContextTaskContext *context = (DeviceContextTaskContext *)malloc(sizeof(DeviceContextTaskContext));
    if (context == NULL) {
        console_error(&csl, "failed to allocate memory for device context task context");
        return NULL;
    }

    context->device_context = device_context;
    context->registration = registration;
    context->access_token = access_token;
    context->task_id = 0;

    // Convert seconds to milliseconds for scheduler
    uint32_t interval_ms = config.device_context_interval * 1000;
    uint32_t initial_delay_ms = config.device_context_interval * 1000; // Start after one interval

    console_info(&csl, "Starting device context service with interval %u ms", interval_ms);

    // Schedule repeating task
    context->task_id = schedule_repeating(initial_delay_ms, interval_ms, device_context_task, context);

    if (context->task_id == 0) {
        console_error(&csl, "failed to schedule device context task");
        free(context);
        return NULL;
    }

    console_debug(&csl, "Successfully scheduled device context task with ID %u", context->task_id);
    return context;
}

void clean_device_context_context(DeviceContextTaskContext *context) {
    console_debug(&csl, "clean_device_context_context called with context: %p", context);
    if (context != NULL) {
        if (context->task_id != 0) {
            console_debug(&csl, "Cancelling device context task %u", context->task_id);
            cancel_task(context->task_id);
        }
        console_debug(&csl, "Freeing device context context %p", context);
        free(context);
    }
}

void clean_device_context(DeviceContext *device_context) {
    if (device_context->site != NULL) {
        if (device_context->site->id != NULL) free(device_context->site->id);
        if (device_context->site->name != NULL) free(device_context->site->name);
        if (device_context->site->mac != NULL) free(device_context->site->mac);
        free(device_context->site);
    }
    free(device_context);
    console_info(&csl, "cleaned device context");
}
