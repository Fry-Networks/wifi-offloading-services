#include "commands.h"
#include "core/console.h"
#include "services/access_token.h"
#include "services/device_info.h"
#include "services/firmware_upgrade.h"
#include "services/mqtt/mqtt.h"
#include "services/registration.h"
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *codename;
    const char *version;
    const char *fry_device_id;
    AccessToken *access_token;
} FirmwareUpdateCommandContext;

static Console csl = {
    .topic = "commands",
};

static FirmwareUpdateCommandContext firmware_update_command_context = {NULL, NULL, NULL, NULL};

char *execute_command(const char *cmd) {
    char buffer[128];
    char *result = NULL;
    size_t result_size = 0;
    FILE *pipe = popen(cmd, "r");
    if (!pipe) {
        console_error(&csl, "Failed to execute command: %s", cmd);
        return strdup("Error executing command");
    }

    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        size_t len = strlen(buffer);
        char *new_result = realloc(result, result_size + len + 1);
        if (!new_result) {
            free(result);
            pclose(pipe);
            return strdup("Memory allocation error");
        }
        result = new_result;
        strcpy(result + result_size, buffer);
        result_size += len;
    }
    pclose(pipe);
    return result ? result : strdup("No output");
}

void commands_callback(struct mosquitto *mosq, const struct mosquitto_message *message) {
    console_debug(&csl, "Received message on commands topic, payload: %s", (char *)message->payload);

    // Parse the JSON payload
    struct json_object *parsed_json;
    struct json_object *command;

    parsed_json = json_tokener_parse((char *)message->payload);
    if (!parsed_json) {
        console_error(&csl, "Failed to parse commands topic payload JSON");
        return;
    }

    if (!json_object_object_get_ex(parsed_json, "command", &command)) {
        console_error(&csl, "Failed to extract command field from commands topic payload JSON");
        json_object_put(parsed_json);
        return;
    }

    const char *command_str = json_object_get_string(command);

    if (strcmp(command_str, "check_firmware_update") == 0) {
        console_info(&csl, "Received firmware update command");
        send_firmware_check_request(firmware_update_command_context.codename, firmware_update_command_context.version,
                                    firmware_update_command_context.fry_device_id,
                                    firmware_update_command_context.access_token);
    } else {
        // Make sure both response_topic and command_id are present in the payload to continue with custom commands
        struct json_object *command_id;
        const char *cmd_id = NULL;
        if (json_object_object_get_ex(parsed_json, "command_id", &command_id)) {
            cmd_id = json_object_get_string(command_id);
        } else {
            console_error(&csl, "Failed to extract command_id field from commands topic payload JSON");
            json_object_put(parsed_json);
            return;
        }

        struct json_object *response_topic;
        const char *resp_topic = NULL;
        if (json_object_object_get_ex(parsed_json, "response_topic", &response_topic)) {
            resp_topic = json_object_get_string(response_topic);
        } else {
            console_error(&csl, "Failed to extract response_topic field from commands topic payload JSON");
            json_object_put(parsed_json);
            return;
        }

        // Execute command and receive its output
        console_info(&csl, "Executing command: %s", command_str);
        char *output = execute_command(command_str);
        console_info(&csl, "Command output: %s", output);

        // Prepare response json
        struct json_object *response_json = json_object_new_object();
        json_object_object_add(response_json, "command_id", json_object_new_string(cmd_id ? cmd_id : "unknown"));
        json_object_object_add(response_json, "result", json_object_new_string(output));

        const char *response_payload = json_object_to_json_string(response_json);

        // Publish
        console_info(&csl, "Publishing response to topic: %s", resp_topic);
        publish_mqtt(mosq, (char *)resp_topic, response_payload, 0);
        json_object_put(response_json);
        free(output);
    }

    // Clean up
    json_object_put(parsed_json);
}

// Subscribe to the commands topic.
// The device will subscribe to "device/<fry_device_id>/command" to receive commands.
void commands_service(struct mosquitto *mosq,
                      DeviceInfo *device_info,
                      Registration *registration,
                      AccessToken *access_token) {
    // Init firmware update command context
    firmware_update_command_context.codename = device_info->name;
    firmware_update_command_context.version = device_info->os_version;
    firmware_update_command_context.fry_device_id = registration->fry_device_id;
    firmware_update_command_context.access_token = access_token;

    // Subscribe to the commands topic
    char commands_topic[256];
    snprintf(commands_topic, sizeof(commands_topic), "device/%s/command", registration->fry_device_id);
    subscribe_mqtt(mosq, commands_topic, 1, commands_callback);
}
