#include "registration.h"
#include "core/console.h"
#include "http/http-requests.h"
#include "services/config/config.h"
#include <json-c/json.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static Console csl = {
    .topic = "registration",
};

#define REGISTER_ENDPOINT "access/register"
#define DEVICE_REGISTRATION_FILE "registration.json"

// Writes a json file with the device registration
void save_device_registration(char *device_registration_json) {
    char registration_file_path[256];
    snprintf(registration_file_path, sizeof(registration_file_path), "%s/%s", config.data_path,
             DEVICE_REGISTRATION_FILE);

    FILE *file = fopen(registration_file_path, "w");
    if (file == NULL) {
        console_error(&csl, "failed to open device registration file for writing; did not save registration");
        return;
    }

    fprintf(file, "%s", device_registration_json);
    fclose(file);
}

// Reads a json file and returns the device registration
char *read_device_registration() {
    char registration_file_path[256];
    snprintf(registration_file_path, sizeof(registration_file_path), "%s/%s", config.data_path,
             DEVICE_REGISTRATION_FILE);

    FILE *file = fopen(registration_file_path, "r");
    if (file == NULL) {
        console_debug(&csl, "failed to open device registration file");
        return NULL;
    }

    // Get file size to allocate memory
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *device_registration = malloc(file_size + 1);
    if (device_registration == NULL) {
        console_error(&csl, "failed to allocate memory for device registration");
        fclose(file);
        return NULL;
    }

    // Read file
    fread(device_registration, 1, file_size, file);
    device_registration[file_size] = '\0';

    fclose(file);

    return device_registration;
}

Registration *parse_device_registration(const char *device_registration_json) {
    struct json_object *parsed_registration;
    struct json_object *fry_device_id;
    struct json_object *access_key;

    parsed_registration = json_tokener_parse(device_registration_json);
    if (parsed_registration == NULL) {
        // JSON parsing failed
        console_error(&csl, "failed to parse device registration JSON data");
        return NULL;
    }

    if (!json_object_object_get_ex(parsed_registration, "fry_device_id", &fry_device_id)) {
        console_error(&csl, "failed to get fry_device_id from device registration");
        json_object_put(parsed_registration);
        return NULL;
    }

    if (!json_object_object_get_ex(parsed_registration, "access_key", &access_key)) {
        console_error(&csl, "failed to get access_key from device registration");
        json_object_put(parsed_registration);
        return NULL;
    }

    Registration *registration = malloc(sizeof(Registration));
    if (registration == NULL) {
        console_error(&csl, "failed to allocate memory for registration");
        json_object_put(parsed_registration);
        return NULL;
    }

    registration->fry_device_id = strdup(json_object_get_string(fry_device_id));
    registration->access_key = strdup(json_object_get_string(access_key));
    json_object_put(parsed_registration);
    return registration;
}

Registration *init_registration(char *mac, char *model, char *brand, char *openwisp_device_id) {
    Registration *registration;

    bool is_odyssey = strcmp(model, "Odyssey") == 0;

    char *registration_str = read_device_registration();
    if (registration_str != NULL) {
        registration = parse_device_registration(registration_str);
        free(registration_str);
        if (registration->fry_device_id != NULL && registration->access_key != NULL) {
            return registration;
        }
    }

    console_info(&csl, "device is not registered, attempting to register ...");

    // Url
    char register_url[256];
    snprintf(register_url, sizeof(register_url), "%s/%s", config.accounting_api, REGISTER_ENDPOINT);

    // Request body
    json_object *json_body = json_object_new_object();
    json_object_object_add(json_body, "mac", json_object_new_string(mac));
    json_object_object_add(json_body, "model", json_object_new_string(model));
    json_object_object_add(json_body, "brand", json_object_new_string(brand));

    if (!is_odyssey) {
        json_object_object_add(json_body, "openwisp_device_id", json_object_new_string(openwisp_device_id));
    }

    // json_object_object_add(json_body, "openwisp_device_id", json_object_new_string(openwisp_device_id));
    const char *body = json_object_to_json_string(json_body);
    console_debug(&csl, "register device request body %s", body);

    HttpPostOptions options = {
        .url = register_url,
        .body_json_str = body,
    };

    HttpResult result = http_post(&options);
    json_object_put(json_body);

    if (result.is_error) {
        console_error(&csl, "failed to register device, error: %s", result.error);
        return false;
    }

    if (result.response_buffer == NULL) {
        console_error(&csl, "failed to register device, no response received");
        return false;
    }

    // Parse response
    registration = parse_device_registration(result.response_buffer);
    if (registration->fry_device_id == NULL || registration->access_key == NULL) {
        console_error(&csl, "failed to register device, no device id or access key received");
        free(result.response_buffer);
        return false;
    }

    // Save registration
    save_device_registration(result.response_buffer);
    console_info(&csl, "registration initialized");

    // Cleanup
    free(result.response_buffer);

    return registration;
}

void clean_registration(Registration *registration) {
    if (registration == NULL) {
        console_debug(&csl, "no registration found, skipping cleanup");
        return;
    }

    if (registration->fry_device_id != NULL) {
        free(registration->fry_device_id);
        registration->fry_device_id = NULL;
    }

    if (registration->access_key != NULL) {
        free(registration->access_key);
        registration->access_key = NULL;
    }

    console_info(&csl, "cleaned registration");
}
