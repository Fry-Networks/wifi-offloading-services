#include "speedtest.h"
#include "core/console.h"
#include "core/uloop_scheduler.h"
#include "services/config/config.h"
#include "services/gen_id.h"
#include "services/mqtt/mqtt.h"
#include "services/registration.h"
#include <curl/curl.h>
#include <json-c/json.h>
#include <mosquitto.h>
#include <services/access_token.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <time.h>

#define SPEEDTEST_ENDPOINT "monitoring/speedtest"

typedef struct {
    bool is_error;
    double upload_speed_mbps;
    double download_speed_mbps;
} SpeedTestResult;

typedef struct {
    bool is_error;
    double speed_mbps;
} TestResult;

static Console csl = {
    .topic = "speed test",
};

float get_average_latency(const char *hostname) {
    char command[256];
    snprintf(command, sizeof(command), "ping -c %d %s", config.speed_test_latency_attempts, hostname);

    FILE *fp = popen(command, "r");
    if (fp == NULL) {
        console_error(&csl, "Failed to run ping command");
        return -1;
    }

    char output[1024];
    float total_latency = 0.0;
    int executed_pings = 0;

    while (fgets(output, sizeof(output), fp) != NULL) {
        char *time_str = strstr(output, "time=");
        if (time_str != NULL) {
            float latency;
            if (sscanf(time_str, "time=%f", &latency) == 1) {
                total_latency += latency;
                executed_pings++;
            }
        }
    }

    pclose(fp);

    if (executed_pings > 0) {
        return total_latency / executed_pings;
    } else {
        return -1;
    }
}

void speedtest_task(void *task_context) {
    SpeedTestTaskContext *context = (SpeedTestTaskContext *)task_context;

    console_debug(&csl, "Starting speedtest task");
    int interval = 0;
    int failed = 0;
    double upload_speed = 0.0;
    double download_speed = 0.0;
    float latency = get_average_latency("www.google.com");
    console_debug(&csl, "Average latency: %.2f ms", latency);
    time_t now;
    time(&now);
    char measurementid[256];
    const double upload_default = 0.0;
    const double download_default = 0.0;
    generate_id(measurementid, sizeof(measurementid), context->registration->fry_device_id, now);
    console_debug(&csl, "Measurement ID for speedtest: %s", measurementid);
    json_object *speedtest_data = json_object_new_object();
    json_object_object_add(speedtest_data, "measurement_id", json_object_new_string(measurementid));
    json_object_object_add(speedtest_data, "device_id", json_object_new_string(context->registration->fry_device_id));
    json_object_object_add(speedtest_data, "timestamp", json_object_new_int(now));
    json_object_object_add(speedtest_data, "upload_speed", json_object_new_double(upload_default));
    json_object_object_add(speedtest_data, "download_speed", json_object_new_double(download_default));
    json_object_object_add(speedtest_data, "latency", json_object_new_double(latency));
    const char *speedtest_data_str = json_object_to_json_string(speedtest_data);

    console_info(&csl, "publishing speedtest to monitoring/speedtest");
    publish_mqtt(context->mosq, "monitoring/speedtest", speedtest_data_str, 0);

    json_object_put(speedtest_data);

    // Schedule monitoring_task to rerun later
    unsigned int seed = time(0);
    // No manual rescheduling needed - repeating tasks auto-reschedule
}

SpeedTestTaskContext *speedtest_service(struct mosquitto *mosq, Registration *registration, AccessToken *access_token) {
    if (config.speed_test_enabled == 0) {
        console_info(&csl, "Speedtest service is disabled by config");
        return NULL;
    }

    SpeedTestTaskContext *context = (SpeedTestTaskContext *)malloc(sizeof(SpeedTestTaskContext));
    if (context == NULL) {
        console_error(&csl, "failed to allocate memory for speedtest task context");
        return NULL;
    }

    context->mosq = mosq;
    context->registration = registration;
    context->access_token = access_token;
    context->task_id = 0;

    // Use a random interval between configured min/max
    unsigned int seed = time(0);
    uint32_t random_interval =
        rand_r(&seed) % (config.speed_test_maximum_interval - config.speed_test_minimum_interval + 1) +
        config.speed_test_minimum_interval;
    uint32_t interval_ms = random_interval * 1000; // Convert to milliseconds
    uint32_t initial_delay_ms = interval_ms;

    console_info(&csl, "Starting speedtest service with interval %u ms", interval_ms);

    // Schedule repeating task
    context->task_id = schedule_repeating(initial_delay_ms, interval_ms, speedtest_task, context);

    if (context->task_id == 0) {
        console_error(&csl, "failed to schedule speedtest task");
        free(context);
        return NULL;
    }

    console_debug(&csl, "Successfully scheduled speedtest task with ID %u", context->task_id);
    return context;
}

void clean_speedtest_context(SpeedTestTaskContext *context) {
    console_debug(&csl, "clean_speedtest_context called with context: %p", context);
    if (context != NULL) {
        if (context->task_id != 0) {
            console_debug(&csl, "Cancelling speedtest task %u", context->task_id);
            cancel_task(context->task_id);
        }
        console_debug(&csl, "Freeing speedtest context %p", context);
        free(context);
    }
}
