#include "device_info.h"
#include "core/console.h"
#include "core/script_runner.h"
#include "services/config/config.h"
#include "services/did-key.h"
#include "services/exit_handler.h"
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unistd.h>

#define OS_VERSION_FILE "/etc/openwrt_release"
#define PACKAGE_VERSION_FILE "/etc/fry-os-services/VERSION"
#define ID_LENGTH 37
#define MAX_RETRIES 50
#define DEVICE_PROFILE_FILE "/etc/fry-os/device.json"

static Console csl = {
    .topic = "device-info",
};

char *get_os_version() {
    if (config.dev_env) {
        return strdup("23.0.4");
    }

    FILE *file = fopen(OS_VERSION_FILE, "r");
    if (file == NULL) {
        console_error(&csl, "error opening file");
        return NULL;
    }

    char *os_version = NULL;

    int MAX_LINE_LENGTH = 256;
    char line[MAX_LINE_LENGTH];
    char distrib_id[MAX_LINE_LENGTH];
    char distrib_release[MAX_LINE_LENGTH];

    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, "DISTRIB_ID", 10) == 0) {
            sscanf(line, "DISTRIB_ID='%[^']'", distrib_id);
        } else if (strncmp(line, "DISTRIB_RELEASE", 15) == 0) {
            sscanf(line, "DISTRIB_RELEASE='%[^']'", distrib_release);
        }
    }

    if (strchr(distrib_release, '\n') != NULL) {
        distrib_release[strcspn(distrib_release, "\n")] = 0;
    }

    fclose(file);

    // Allocate memory and copy the version string
    if (*distrib_release != '\0') { // Check if distrib_release is not empty
        os_version = strdup(distrib_release);
        if (os_version == NULL) {
            console_error(&csl, "memory allocation failed for os_version");
            fclose(file);
            return NULL;
        }
    } else {
        console_error(&csl, "os_version is empty");
    }

    console_debug(&csl, "os_version is: %s", os_version);

    return os_version;
}

char *get_os_services_version() {
    if (config.dev_env == 1) {
        return strdup("1.0.0");
    }

    FILE *file = fopen(PACKAGE_VERSION_FILE, "r");
    if (file == NULL) {
        console_error(&csl, "error opening services version file");
        return NULL;
    }

    int MAX_LINE_LENGTH = 256;
    char *os_services_version = NULL;
    char version[MAX_LINE_LENGTH];

    if (fgets(version, MAX_LINE_LENGTH, file) == NULL) {
        console_error(&csl, "failed to read services version");
        fclose(file);
        return NULL; // Handle failed read attempt
    }

    if (strchr(version, '\n') != NULL) {
        version[strcspn(version, "\n")] = 0;
    }

    fclose(file);

    // Allocate memory for the version string and return
    os_services_version = strdup(version);
    if (os_services_version == NULL) {
        console_error(&csl, "memory allocation failed for services version");
        return NULL;
    }

    console_debug(&csl, "services version is: %s", os_services_version);

    return os_services_version;
}

char *get_mac_for_model(const char *device_name) {
    char script_file[256];
    
    snprintf(script_file, sizeof(script_file), "%s%s", config.scripts_path, "/get-mac.sh");
    
    // Set environment variable for the script (using device name)
    if (device_name) {
        setenv("FRY_MODEL", device_name, 1);
    } else {
        setenv("FRY_MODEL", "unknown", 1);
    }
    
    console_info(&csl, "Getting MAC address for device: %s", device_name ? device_name : "unknown");
    
    char *mac = run_script(script_file);
    if (mac && strchr(mac, '\n') != NULL) {
        mac[strcspn(mac, "\n")] = 0;
    }

    console_debug(&csl, "mac address is: %s", mac ? mac : "NULL");

    return mac;
}

DeviceProfile get_device_profile() {
    DeviceProfile device_profile = {0};

    if (config.dev_env) {
        device_profile.name = strdup("Hemera");
        device_profile.brand = strdup("Fry");
        device_profile.model = strdup("Genesis");
        return device_profile;
    }

    FILE *file = fopen(DEVICE_PROFILE_FILE, "r");
    if (file == NULL) {
        console_error(&csl, "error opening device info file");
        return device_profile;
    }

    // Read the file into a string
    fseek(file, 0, SEEK_END);
    long fsize = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *json_string = malloc(fsize + 1);
    fread(json_string, 1, fsize, file);
    fclose(file);
    json_string[fsize] = 0;

    // Parse the string into a json object
    struct json_object *parsed_json = json_tokener_parse(json_string);
    free(json_string);

    struct json_object *name;
    struct json_object *brand;
    struct json_object *model;

    json_object_object_get_ex(parsed_json, "name", &name);
    json_object_object_get_ex(parsed_json, "brand", &brand);
    json_object_object_get_ex(parsed_json, "model", &model);

    // Copy the values into the device_info struct
    device_profile.name = strdup(json_object_get_string(name));
    device_profile.brand = strdup(json_object_get_string(brand));
    device_profile.model = strdup(json_object_get_string(model));

    // Free the JSON object
    json_object_put(parsed_json);

    console_debug(&csl, "device identifiers are: %s, %s, %s", device_profile.name, device_profile.brand,
                  device_profile.model);

    return device_profile;
}

char *get_id() {

    DeviceProfile device_profile = get_device_profile();

    if (strcmp(device_profile.model, "Odyssey") == 0) {
        console_info(&csl, "Device is Odyssey, skipping openwisp UUID retrieval");
        return NULL;
    }

    char script_file[256];
    snprintf(script_file, sizeof(script_file), "%s%s", config.scripts_path, "/get-uuid.sh");
    char *id = NULL;
    int retry_count = 0;

    // Loop indefinitely until a valid UUID is obtained
    while (retry_count < MAX_RETRIES) {
        id = run_script(script_file);
        if (id != NULL && strlen(id) > 1 && strncmp(id, "uci", 3) != 0) {
            if (strchr(id, '\n') != NULL) {
                id[strcspn(id, "\n")] = 0;
            }

            console_debug(&csl, "UUID found; took %d attempts.", retry_count + 1);
            console_debug(&csl, "UUID is: %s", id);

            break; // Exit the loop if a valid UUID is obtained
        }

        console_debug(&csl, "retrying to obtain UUID...");
        sleep(5); // Wait for 5 seconds before retrying
        retry_count++;
    }
    if (retry_count == MAX_RETRIES) {
        console_error(&csl, "unable to obtain UUID after %d attempts. Exiting.", MAX_RETRIES);
        cleanup_and_exit(1, "Unable to obtain device UUID after maximum retries");
    }

    return id;
}

char *get_public_ip() {
    char script_file[256];
    snprintf(script_file, sizeof(script_file), "%s%s", config.scripts_path, "/get-public-ip.sh");
    char *public_ip = run_script(script_file);
    if (strchr(public_ip, '\n') != NULL) {
        public_ip[strcspn(public_ip, "\n")] = 0;
    }

    console_debug(&csl, "public ip: %s", public_ip);

    return public_ip;
}

char *get_os_name() {
    char script_file[256];
    snprintf(script_file, sizeof(script_file), "%s%s", config.scripts_path, "/get-osname.sh");
    char *os_name = run_script(script_file);
    if (strchr(os_name, '\n') != NULL) {
        os_name[strcspn(os_name, "\n")] = 0;
    }

    return os_name;
}

// Function to get the architecture and subtarget of the device
char *get_arch() {
    if (config.dev_env) {
        return strdup("x86_64");
    }

    static char arch[64] = {0};
    char distrib_arch[32] = {0};
    char distrib_target[64] = {0};

    FILE *f = fopen("/etc/openwrt_release", "r");
    if (f == NULL) {
        console_error(&csl, "error opening file");
        return NULL;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "DISTRIB_ARCH=") == line) {
            char *start = strchr(line, '\'');
            if (start) {
                start++;
                char *end = strchr(start, '\'');
                if (end) {
                    size_t len = end - start;
                    if (len < sizeof(distrib_arch)) {
                        strncpy(distrib_arch, start, len);
                        distrib_arch[len] = '\0';
                    }
                }
            }
        }

        if (strstr(line, "DISTRIB_TARGET=") == line) {
            char *start = strchr(line, '\'');
            if (start) {
                start++;
                char *end = strchr(start, '\'');
                if (end) {
                    size_t len = end - start;
                    if (len < sizeof(distrib_target)) {
                        strncpy(distrib_target, start, len);
                        distrib_target[len] = '\0';
                    }
                }
            }
        }
    }

    fclose(f);

    if (distrib_arch[0] == '\0' || distrib_target[0] == '\0') {
        console_error(&csl, "missing fields in /etc/openwrt_release");
        return NULL;
    }

    char *subtarget = strchr(distrib_target, '/');
    if (!subtarget || *(subtarget + 1) == '\0') {
        console_error(&csl, "invalid DISTRIB_TARGET format");
        return NULL;
    }

    subtarget++;

    snprintf(arch, sizeof(arch), "%s_%s", distrib_arch, subtarget);
    return arch;
}

DeviceInfo *init_device_info() {
    DeviceInfo *device_info = malloc(sizeof(DeviceInfo));
    device_info->os_version = get_os_version();
    device_info->os_services_version = get_os_services_version();
    
    // Get device profile first to determine MAC interface
    DeviceProfile device_profile = get_device_profile();
    device_info->name = device_profile.name;
    device_info->model = device_profile.model;
    device_info->brand = device_profile.brand;
    
    // Now get MAC with device name information
    device_info->mac = get_mac_for_model(device_profile.name);

    device_info->arch = get_arch();

    device_info->device_id = get_id();
    device_info->public_ip = get_public_ip();
    device_info->os_name = get_os_name();
    device_info->did_public_key = get_did_public_key_or_generate_keypair();

    console_info(&csl, "device info initialized");

    return device_info;
}

void clean_device_info(DeviceInfo *device_info) {
    free(device_info->mac);
    free(device_info->name);
    free(device_info->brand);
    free(device_info->model);
    free(device_info->os_name);
    free(device_info->os_version);
    free(device_info->os_services_version);
    free(device_info->device_id);
    free(device_info->public_ip);
    free(device_info->did_public_key);
    console_info(&csl, "cleaned device info");
}
