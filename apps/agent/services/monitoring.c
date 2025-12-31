#include "monitoring.h"
#include "core/console.h"
#include "core/script_runner.h"
#include "core/uloop_scheduler.h"
#include "services/config/config.h"
#include "services/device_info.h"
#include "services/gen_id.h"
#include "services/mqtt/mqtt.h"
#include "services/registration.h"
#include <json-c/json.h>
#include <mosquitto.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static Console csl = {
    .topic = "monitoring",
};

typedef struct {
    int wifi_clients;
    unsigned long memory_total;
    unsigned long memory_free;
    unsigned long memory_used;
    unsigned long memory_shared;
    unsigned long memory_buffered;
    int cpu_count;
    float cpu_load;
    int cpu_load_percent;
    unsigned long disk_used;
    unsigned long disk_size;
    unsigned long disk_available;
    int disk_used_percent;
    int radio_count;
    int radio_live;
} DeviceData;

void parse_output(const char *output, DeviceData *info) {
    struct {
        const char *key;
        const char *format;
        void *value;
    } mappings[] = {
        {"wifi_clients:", " %d", &info->wifi_clients},
        {"memory_total:", " %lu", &info->memory_total},
        {"memory_free:", " %lu", &info->memory_free},
        {"memory_used:", " %lu", &info->memory_used},
        {"memory_shared:", " %lu", &info->memory_shared},
        {"memory_buffered:", " %lu", &info->memory_buffered},
        {"cpu_count:", " %d", &info->cpu_count},
        {"cpu_load:", " %f", &info->cpu_load},
        {"cpu_load_percent:", " %d", &info->cpu_load_percent},
        {"disk_used:", " %lu", &info->disk_used},
        {"disk_size:", " %lu", &info->disk_size},
        {"disk_available:", " %lu", &info->disk_available},
        {"disk_used_percent:", " %d", &info->disk_used_percent},
        {"radio_count:", " %d", &info->radio_count},
        {"radio_live:", " %d", &info->radio_live},
    };

    const int mappings_count = sizeof(mappings) / sizeof(mappings[0]);
    char *line = strtok(output, "\n");
    while (line != NULL) {
        for (int i = 0; i < mappings_count; ++i) {
            if (strstr(line, mappings[i].key) == line) {
                sscanf(line + strlen(mappings[i].key), mappings[i].format, mappings[i].value);
                break;
            }
        }
        line = strtok(NULL, "\n");
    }
}

json_object *createjson(DeviceData *device_data,
                        json_object *jobj,
                        int timestamp,
                        Registration *registration,
                        char *measurementid,
                        char *os_name,
                        char *os_version,
                        char *os_services_version,
                        char *public_ip) {
    json_object_object_add(jobj, "os_name", json_object_new_string(os_name));
    json_object_object_add(jobj, "os_version", json_object_new_string(os_version));
    json_object_object_add(jobj, "os_services_version", json_object_new_string(os_services_version));
    json_object_object_add(jobj, "public_ip", json_object_new_string(public_ip));
    json_object_object_add(jobj, "measurement_id", json_object_new_string(measurementid));
    json_object_object_add(jobj, "device_id", json_object_new_string(registration->fry_device_id));
    json_object_object_add(jobj, "timestamp", json_object_new_int(timestamp));
    json_object_object_add(jobj, "wifi_clients", json_object_new_int(device_data->wifi_clients));
    json_object_object_add(jobj, "memory_total", json_object_new_int64(device_data->memory_total));
    json_object_object_add(jobj, "memory_free", json_object_new_int64(device_data->memory_free));
    json_object_object_add(jobj, "memory_used", json_object_new_int64(device_data->memory_used));
    json_object_object_add(jobj, "memory_shared", json_object_new_int64(device_data->memory_shared));
    json_object_object_add(jobj, "memory_buffered", json_object_new_int64(device_data->memory_buffered));
    json_object_object_add(jobj, "cpu_count", json_object_new_int(device_data->cpu_count));
    json_object_object_add(jobj, "cpu_load", json_object_new_double(device_data->cpu_load));
    json_object_object_add(jobj, "cpu_load_percent", json_object_new_int(device_data->cpu_load_percent));
    json_object_object_add(jobj, "disk_used", json_object_new_int64(device_data->disk_used));
    json_object_object_add(jobj, "disk_size", json_object_new_int64(device_data->disk_size));
    json_object_object_add(jobj, "disk_available", json_object_new_int64(device_data->disk_available));
    json_object_object_add(jobj, "disk_used_percent", json_object_new_int(device_data->disk_used_percent));
    json_object_object_add(jobj, "radio_count", json_object_new_int(device_data->radio_count));
    json_object_object_add(jobj, "radio_live", json_object_new_int(device_data->radio_live));
    return jobj;
}

void monitoring_task(void *task_context) {
    MonitoringTaskContext *context = (MonitoringTaskContext *)task_context;

    time_t now;
    time(&now);
    DeviceData device_data;
    char script_file[256];
    snprintf(script_file, sizeof(script_file), "%s%s", config.scripts_path, "/retrieve-data.lua");
    char *output = run_script(script_file);
    if (output == NULL) {
        console_error(&csl, "failed to run script %s", script_file);
        return;
    }
    parse_output(output, &device_data);
    free(output);

    context->os_name = get_os_name();
    context->os_version = get_os_version();
    context->os_services_version = get_os_services_version();
    context->public_ip = get_public_ip();

    json_object *json_device_data = json_object_new_object();
    char measurementid[256];
    generate_id(measurementid, sizeof(measurementid), context->registration->fry_device_id, now);
    console_debug(&csl, "measurement ID for deviceData: %s", measurementid);
    createjson(&device_data, json_device_data, now, context->registration, measurementid, context->os_name,
               context->os_version, context->os_services_version, context->public_ip);

    const char *device_data_str = json_object_to_json_string(json_device_data);

    console_debug(&csl, "device data: %s", device_data_str);
    console_info(&csl, "publishing device data to monitoring/device-data");
    publish_mqtt(context->mosq, "monitoring/device-data", device_data_str, 0);

    json_object_put(json_device_data);

    // Free the dynamically allocated strings from this task execution
    free(context->os_name);
    free(context->os_version);
    free(context->os_services_version);
    free(context->public_ip);

    // Reset pointers to avoid double-free in cleanup
    context->os_name = NULL;
    context->os_version = NULL;
    context->os_services_version = NULL;
    context->public_ip = NULL;

    // No manual rescheduling needed - repeating tasks auto-reschedule
}

MonitoringTaskContext *monitoring_service(struct mosquitto *mosq, Registration *registration) {
    if (config.monitoring_enabled == 0) {
        console_info(&csl, "monitoring service is disabled by config param");
        return NULL;
    }

    MonitoringTaskContext *context = (MonitoringTaskContext *)malloc(sizeof(MonitoringTaskContext));
    if (context == NULL) {
        console_error(&csl, "failed to allocate memory for monitoring task context");
        return NULL;
    }

    context->mosq = mosq;
    context->registration = registration;
    context->os_name = NULL;
    context->os_version = NULL;
    context->os_services_version = NULL;
    context->public_ip = NULL;
    context->task_id = 0;

    // Use a random interval between 5-10 minutes as before
    uint32_t min_interval = 5 * 60 * 1000;  // 5 minutes in ms
    uint32_t max_interval = 10 * 60 * 1000; // 10 minutes in ms
    uint32_t interval_ms = min_interval + (rand() % (max_interval - min_interval));
    uint32_t initial_delay_ms = interval_ms;

    console_info(&csl, "Starting monitoring service with interval %u ms", interval_ms);

    // Schedule repeating task
    context->task_id = schedule_repeating(initial_delay_ms, interval_ms, monitoring_task, context);

    if (context->task_id == 0) {
        console_error(&csl, "failed to schedule monitoring task");
        free(context);
        return NULL;
    }

    console_debug(&csl, "Successfully scheduled monitoring task with ID %u", context->task_id);
    return context;
}

void clean_monitoring_context(MonitoringTaskContext *context) {
    console_debug(&csl, "clean_monitoring_context called with context: %p", context);
    if (context != NULL) {
        if (context->task_id != 0) {
            console_debug(&csl, "Cancelling monitoring task %u", context->task_id);
            cancel_task(context->task_id);
        }

        // Free any remaining allocated strings (in case task was cancelled mid-execution)
        if (context->os_name) free(context->os_name);
        if (context->os_version) free(context->os_version);
        if (context->os_services_version) free(context->os_services_version);
        if (context->public_ip) free(context->public_ip);

        console_debug(&csl, "Freeing monitoring context %p", context);
        free(context);
    }
}
