#include "nds.h"
#include "core/console.h"
#include "core/script_runner.h"
#include "core/uloop_scheduler.h"
#include "services/config/config.h"
#include "services/device-context.h"
#include "services/mqtt/mqtt.h"
#include <errno.h>
#include <fcntl.h>
#include <json-c/json_object.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NDS_FIFO "nds-fifo"
#define NDS_FIFO_BUFFER_SIZE 512
#define NDS_EVENTS_ARRAY_SIZE 10
#define SET_BINAUTH_SCRIPT "nds-set-binauth.lua"
#define BINAUTH_SCRIPT "nds-binauth.sh"

static Console csl = {
    .topic = "nds",
};

// @todo Make sure OpenWISP is not trying to overwrite the binauth setting in the OpenNDS config file
void init_nds_binauth() {
    // Build the command
    char binauth_script_path[256];
    snprintf(binauth_script_path, sizeof(binauth_script_path), "%s/%s", config.scripts_path, BINAUTH_SCRIPT);

    char command[512];
    snprintf(command, sizeof(command), "%s/%s %s", config.scripts_path, SET_BINAUTH_SCRIPT, binauth_script_path);

    // Run the script
    char *output = run_script(command);
    console_debug(&csl, "Script output: %s", output);

    // Clean up
    free(output);
}

int init_nds_fifo() {
    // Create the dir within the /tmp folder and FIFO file if it doesn't already exist
    struct stat st = {0};
    char fifo_dir[256];
    char fifo_file[256];

    snprintf(fifo_dir, sizeof(fifo_dir), "/tmp/fry-os-services");
    snprintf(fifo_file, sizeof(fifo_file), "%s/%s", "/tmp/fry-os-services", NDS_FIFO);

    // Check if directory exists
    if (stat(fifo_dir, &st) == -1) {
        // Directory does not exist, create it
        if (mkdir(fifo_dir, 0700) == -1) {
            console_error(&csl, "failed to create nds fifo directory");
            return -1;
        }
        console_debug(&csl, "nds fifo directory created: %s", fifo_dir);
    } else {
        console_debug(&csl, "nds fifo directory already exists: %s", fifo_dir);
    }

    // Create the FIFO file if it doesn't exist
    if (mkfifo(fifo_file, 0666) == -1) {
        if (errno != EEXIST) {
            console_error(&csl, "failed to create nds fifo file");
            return -1;
        }
    }

    int fifo_fd = open(fifo_file, O_RDONLY | O_NONBLOCK);
    if (fifo_fd == -1) {
        console_error(&csl, "failed to open nds fifo file");
        return -1;
    }

    console_info(&csl, "nds fifo file opened, fifo_fd: %d", fifo_fd);

    return fifo_fd;
}

NdsClient *init_nds_client() {
    NdsClient *client = (NdsClient *)malloc(sizeof(NdsClient));
    if (client == NULL) {
        console_error(&csl, "failed to allocate memory for nds client");
        return NULL;
    }

    client->opennds_installed = false;
    client->fifo_fd = -1;

    if (config.dev_env) {
        return client;
    }

    // Check if OpenNDS is installed
    char opennds_check_command[256];
    snprintf(opennds_check_command, sizeof(opennds_check_command), "opkg list-installed | grep opennds");
    client->opennds_installed = system(opennds_check_command) == 0;

    if (!client->opennds_installed) {
        console_warn(&csl, "OpenNDS is not installed");
        return client;
    }

    // Initialize the FIFO
    client->fifo_fd = init_nds_fifo();

    // Configure binauth
    init_nds_binauth();

    return client;
}

void nds_task(void *task_context) {
    console_info(&csl, "Running nds task");

    NdsTaskContext *ctx = (NdsTaskContext *)task_context;
    char buffer[NDS_FIFO_BUFFER_SIZE];

    // Read all available data from the FIFO
    // Each event is expected to be on a separate line
    ssize_t bytesRead = read(ctx->client->fifo_fd, buffer, sizeof(buffer) - 1);
    console_debug(&csl, "Read %ld bytes from fifo", bytesRead);
    if (bytesRead > 0) {
        // Null-terminate the buffer to make it a valid C-string
        buffer[bytesRead] = '\0';

        // Process the data from FIFO
        console_debug(&csl, "Received from fifo: %s", buffer);

        // Create a new JSON array to accumulate events
        json_object *events_array = json_object_new_array();

        int events_count = 0;

        char *line = strtok(buffer, "\n");
        while (line != NULL) {
            // Add gateway_mac to the event string:
            char event_with_mac[1024];
            snprintf(event_with_mac, sizeof(event_with_mac), "%s, gatewaymac=%s", line, ctx->device_info->mac);

            // Add to array
            json_object_array_add(events_array, json_object_new_string(event_with_mac));
            events_count++;

            // Get next line
            line = strtok(NULL, "\n");
        }

        // Publish array
        if (events_count > 0) {
            const char *json_payload_str = json_object_to_json_string(events_array);

            // Publish accounting event (backend is subscribed to this)
            // publish_mqtt(ctx->mosq, "accounting/nds", json_payload_str, 0);
            int publish_rc = mosquitto_publish(ctx->mosq, NULL, "accounting/nds", strlen(json_payload_str),
                                               json_payload_str, 0, false);
            if (publish_rc != MOSQ_ERR_SUCCESS) {
                console_error(&csl, "Failed to publish accounting/nds: %s", mosquitto_strerror(publish_rc));
            }

            // Publish site events (other routers that are part of the same site are subscribed to this)
            if (ctx->site != NULL && ctx->site->id != NULL) {
                char site_topic[256];
                snprintf(site_topic, sizeof(site_topic), "site/%s/clients", ctx->site->id);
                publish_mqtt(ctx->mosq, site_topic, json_payload_str, 0);
            }
        }

        json_object_put(events_array);
    } else if (bytesRead == 0) {
        console_debug(&csl, "No data read from FIFO");
    } else if (bytesRead == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
        // Handle actual error
        console_error(&csl, "Failed to read from site clients fifo");
    }

    // No manual rescheduling needed - repeating tasks auto-reschedule
}

NdsTaskContext *nds_service(Mosq *mosq, Site *site, NdsClient *nds_client, DeviceInfo *device_info) {
    if (config.dev_env) {
        console_info(&csl, "NDS service not started (dev mode)");
        return NULL;
    }

    if (nds_client->opennds_installed == false) {
        console_warn(&csl, "OpenNDS is not installed, skipping nds service");
        return NULL;
    }

    if (nds_client->fifo_fd == -1) {
        console_error(&csl, "nds fifo fd is invalid");
        return NULL;
    }

    NdsTaskContext *ctx = (NdsTaskContext *)malloc(sizeof(NdsTaskContext));
    if (ctx == NULL) {
        console_error(&csl, "failed to allocate memory for nds task context");
        return NULL;
    }

    ctx->mosq = mosq;
    ctx->site = site;
    ctx->client = nds_client;
    ctx->device_info = device_info;
    ctx->task_id = 0;

    // Convert seconds to milliseconds for scheduler
    uint32_t interval_ms = config.nds_interval * 1000;
    uint32_t initial_delay_ms = config.nds_interval * 1000; // Start after one interval

    console_info(&csl, "Starting NDS service with interval %u ms", interval_ms);

    // Schedule repeating task
    ctx->task_id = schedule_repeating(initial_delay_ms, interval_ms, nds_task, ctx);

    if (ctx->task_id == 0) {
        console_error(&csl, "failed to schedule NDS task");
        free(ctx);
        return NULL;
    }

    console_debug(&csl, "Successfully scheduled NDS task with ID %u", ctx->task_id);
    return ctx;
}

void clean_nds_context(NdsTaskContext *context) {
    console_debug(&csl, "clean_nds_context called with context: %p", context);
    if (context != NULL) {
        if (context->task_id != 0) {
            console_debug(&csl, "Cancelling NDS task %u", context->task_id);
            cancel_task(context->task_id);
        }
        console_debug(&csl, "Freeing NDS context %p", context);
        free(context);
    }
}

void clean_nds_fifo(int *nds_fifo_fd) {
    if (nds_fifo_fd == NULL) {
        console_error(&csl, "nds fifo fd is NULL");
        return;
    }

    // Close the FIFO file descriptor if it is valid (non-negative)
    if (*nds_fifo_fd >= 0) {
        if (close(*nds_fifo_fd) == 0) {
            console_info(&csl, "nds fifo closed, nds_fifo_fd: %d", *nds_fifo_fd);
        } else {
            console_error(&csl, "failed to close nds fifo, nds_fifo_fd: %d", *nds_fifo_fd);
        }
        *nds_fifo_fd = -1;
    } else {
        console_warn(&csl, "nds fifo already closed or invalid, nds_fifo_fd: %d", *nds_fifo_fd);
    }

    // Build the FIFO file path and unlink it
    char fifo_path[256];
    if (snprintf(fifo_path, sizeof(fifo_path), "%s/fry-os-services/%s", config.temp_path, NDS_FIFO) >=
        (int)sizeof(fifo_path)) {
        console_error(&csl, "nds fifo file path exceeds buffer size");
        return;
    }

    if (unlink(fifo_path) == 0) {
        console_info(&csl, "nds fifo file unlinked, path: %s", fifo_path);
    } else {
        console_error(&csl, "failed to unlink nds fifo, path: %s", fifo_path);
    }
    unlink(fifo_path);

    console_info(&csl, "cleaned nds fifo");
}
