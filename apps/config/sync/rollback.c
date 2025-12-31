#include "rollback.h"
#include "core/console.h"
#include "renderer/renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <json-c/json.h>
#include <errno.h>

static Console csl = {.topic = "rollback"};

// Rollback paths
#define DEV_ROLLBACK_DIR "./scripts/dev/rollback"
#define PROD_ROLLBACK_DIR "/etc/fry-config/rollback"
#define ROLLBACK_CONFIG_FILE "config.json"

// Section-specific paths
#define WIRELESS_CONFIG_FILE "wireless_config.json"
#define AGENT_CONFIG_FILE "agent_config.json"
#define COLLECTOR_CONFIG_FILE "collector_config.json"
#define CONFIG_CONFIG_FILE "config_config.json"
#define OPENNDS_CONFIG_FILE "opennds_config.json"

// Get rollback directory path
static const char* get_rollback_dir(bool dev_mode) {
    return dev_mode ? DEV_ROLLBACK_DIR : PROD_ROLLBACK_DIR;
}

// Ensure rollback directory exists
static int ensure_rollback_dir(bool dev_mode) {
    const char *rollback_dir = get_rollback_dir(dev_mode);
    
    if (mkdir(rollback_dir, 0755) != 0 && errno != EEXIST) {
        console_error(&csl, "Failed to create rollback directory: %s", rollback_dir);
        return -1;
    }
    
    return 0;
}

// Get section-specific config filename
static const char* get_section_config_filename(const char *section_type, const char *meta_config_name) {
    if (strcmp(section_type, "wireless") == 0) {
        return WIRELESS_CONFIG_FILE;
    } else if (strcmp(section_type, "opennds") == 0) {
        return OPENNDS_CONFIG_FILE;
    } else if (strcmp(section_type, "fry") == 0) {
        if (meta_config_name && strcmp(meta_config_name, "fry-agent") == 0) {
            return AGENT_CONFIG_FILE;
        } else if (meta_config_name && strcmp(meta_config_name, "fry-collector") == 0) {
            return COLLECTOR_CONFIG_FILE;
        } else if (meta_config_name && strcmp(meta_config_name, "fry-config") == 0) {
            return CONFIG_CONFIG_FILE;
        }
    }
    return NULL;
}

// Load successful configuration from rollback file
static char* load_successful_config(bool dev_mode) {
    const char *rollback_dir = get_rollback_dir(dev_mode);
    
    char config_path[512];
    snprintf(config_path, sizeof(config_path), "%s/%s", rollback_dir, ROLLBACK_CONFIG_FILE);
    
    FILE *fp = fopen(config_path, "r");
    if (!fp) {
        console_warn(&csl, "No previous successful config found at %s", config_path);
        return NULL;
    }
    
    // Get file size
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size > MAX_CONFIG_SIZE) {
        console_error(&csl, "Invalid rollback config file size: %ld", file_size);
        fclose(fp);
        return NULL;
    }
    
    char *config_json = malloc(file_size + 1);
    if (!config_json) {
        console_error(&csl, "Failed to allocate memory for rollback config");
        fclose(fp);
        return NULL;
    }
    
    size_t read_size = fread(config_json, 1, file_size, fp);
    config_json[read_size] = '\0';
    fclose(fp);
    
    console_info(&csl, "Loaded successful config for rollback (%zu bytes)", strlen(config_json));
    return config_json;
}

// Save successful configuration
int save_successful_config(const char *config_json, const char *global_hash, bool dev_mode) {
    if (!config_json || !global_hash) {
        console_error(&csl, "Invalid parameters for saving successful config");
        return -1;
    }
    
    // Ensure rollback directory exists
    if (ensure_rollback_dir(dev_mode) != 0) {
        return -1;
    }
    
    const char *rollback_dir = get_rollback_dir(dev_mode);
    
    // Save JSON config
    char config_path[512];
    snprintf(config_path, sizeof(config_path), "%s/%s", rollback_dir, ROLLBACK_CONFIG_FILE);
    
    FILE *fp = fopen(config_path, "w");
    if (!fp) {
        console_error(&csl, "Failed to save rollback config to %s", config_path);
        return -1;
    }
    
    fprintf(fp, "%s", config_json);
    fclose(fp);
    
    console_debug(&csl, "Saved successful config for rollback (%zu bytes)", strlen(config_json));
    return 0;
}

// Extract configuration section from JSON
char* extract_config_section(const char *full_config_json, const char *section_type, const char *meta_config_name) {
    if (!full_config_json || !section_type) {
        return NULL;
    }
    
    json_object *root = json_tokener_parse(full_config_json);
    if (!root) {
        console_error(&csl, "Failed to parse configuration JSON");
        return NULL;
    }
    
    json_object *device_config = NULL;
    if (!json_object_object_get_ex(root, "device_config", &device_config)) {
        console_error(&csl, "No device_config section found in JSON");
        json_object_put(root);
        return NULL;
    }
    
    json_object *section_obj = NULL;
    
    if (strcmp(section_type, "fry") == 0 && meta_config_name) {
        // Extract fry subsection
        json_object *fry_array;
        if (json_object_object_get_ex(device_config, "fry", &fry_array)) {
            int array_len = json_object_array_length(fry_array);
            
            for (int i = 0; i < array_len; i++) {
                json_object *section = json_object_array_get_idx(fry_array, i);
                json_object *meta_config;
                
                if (json_object_object_get_ex(section, "meta_config", &meta_config)) {
                    const char *config_name = json_object_get_string(meta_config);
                    if (config_name && strcmp(config_name, meta_config_name) == 0) {
                        section_obj = section;
                        break;
                    }
                }
            }
        }
    } else {
        // Extract top-level section
        json_object_object_get_ex(device_config, section_type, &section_obj);
    }
    
    if (!section_obj) {
        console_warn(&csl, "Section %s%s%s not found in configuration", 
                    section_type, 
                    meta_config_name ? "/" : "",
                    meta_config_name ? meta_config_name : "");
        json_object_put(root);
        return NULL;
    }
    
    const char *section_json_str = json_object_to_json_string(section_obj);
    char *result = strdup(section_json_str);
    
    json_object_put(root);
    return result;
}

// Save section-specific configuration
int save_successful_config_section(const char *full_config_json, const char *section_type, 
                                  const char *meta_config_name, const char *section_hash, bool dev_mode) {
    if (!full_config_json || !section_type || !section_hash) {
        console_error(&csl, "Invalid parameters for saving section config");
        return -1;
    }
    
    // Ensure rollback directory exists
    if (ensure_rollback_dir(dev_mode) != 0) {
        return -1;
    }
    
    const char *rollback_dir = get_rollback_dir(dev_mode);
    const char *config_filename = get_section_config_filename(section_type, meta_config_name);
    
    if (!config_filename) {
        console_error(&csl, "Unknown section type: %s (meta: %s)", section_type, meta_config_name ? meta_config_name : "null");
        return -1;
    }
    
    // Extract the specific section from the full JSON
    char *section_json = extract_config_section(full_config_json, section_type, meta_config_name);
    if (!section_json) {
        console_warn(&csl, "Could not extract section %s from config", section_type);
        return -1;
    }
    
    // Save section JSON
    char section_path[512];
    snprintf(section_path, sizeof(section_path), "%s/%s", rollback_dir, config_filename);
    
    FILE *fp = fopen(section_path, "w");
    if (!fp) {
        console_error(&csl, "Failed to save section config to %s", section_path);
        free(section_json);
        return -1;
    }
    
    fprintf(fp, "%s", section_json);
    fclose(fp);
    free(section_json);
    
    console_debug(&csl, "Saved successful config for %s%s%s", 
                 section_type,
                 meta_config_name ? "/" : "",
                 meta_config_name ? meta_config_name : "");
    return 0;
}

// Load successful configuration for a specific section
char* load_successful_config_section(const char *section_type, const char *meta_config_name, bool dev_mode) {
    if (!section_type) {
        console_error(&csl, "Invalid section type for loading");
        return NULL;
    }
    
    const char *rollback_dir = get_rollback_dir(dev_mode);
    const char *config_filename = get_section_config_filename(section_type, meta_config_name);
    
    if (!config_filename) {
        console_error(&csl, "Unknown section type: %s", section_type);
        return NULL;
    }
    
    char section_path[512];
    snprintf(section_path, sizeof(section_path), "%s/%s", rollback_dir, config_filename);
    
    FILE *fp = fopen(section_path, "r");
    if (!fp) {
        console_warn(&csl, "No previous successful config found for %s at %s", section_type, section_path);
        return NULL;
    }
    
    // Get file size
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (file_size <= 0 || file_size > MAX_CONFIG_SIZE) {
        console_error(&csl, "Invalid section config file size: %ld", file_size);
        fclose(fp);
        return NULL;
    }
    
    char *section_json = malloc(file_size + 1);
    if (!section_json) {
        console_error(&csl, "Failed to allocate memory for section config");
        fclose(fp);
        return NULL;
    }
    
    size_t read_size = fread(section_json, 1, file_size, fp);
    section_json[read_size] = '\0';
    fclose(fp);
    
    console_debug(&csl, "Loaded successful config for %s%s%s (%zu bytes)", 
                 section_type,
                 meta_config_name ? "/" : "",
                 meta_config_name ? meta_config_name : "",
                 strlen(section_json));
    return section_json;
}

// Helper functions for JSON manipulation
static bool replace_json_section(json_object *root, const char *section_name, const char *section_json) {
    json_object *section_obj = json_tokener_parse(section_json);
    if (!section_obj) {
        console_error(&csl, "Failed to parse section JSON for %s", section_name);
        return false;
    }
    
    // Get device_config
    json_object *device_config;
    if (!json_object_object_get_ex(root, "device_config", &device_config)) {
        console_error(&csl, "No device_config section found in root JSON");
        json_object_put(section_obj);
        return false;
    }
    
    // Replace or add the section
    json_object_object_add(device_config, section_name, section_obj);
    
    console_debug(&csl, "Replaced section: %s", section_name);
    return true;
}

static bool replace_json_fry_section(json_object *root, const char *fry_service, const char *section_json) {
    json_object *device_config;
    if (!json_object_object_get_ex(root, "device_config", &device_config)) {
        console_error(&csl, "No device_config section found in root JSON");
        return false;
    }
    
    json_object *fry_array;
    if (!json_object_object_get_ex(device_config, "fry", &fry_array)) {
        // Create fry array if it doesn't exist
        fry_array = json_object_new_array();
        json_object_object_add(device_config, "fry", fry_array);
    }
    
    json_object *service_obj = json_tokener_parse(section_json);
    if (!service_obj) {
        console_error(&csl, "Failed to parse fry section JSON for %s", fry_service);
        return false;
    }
    
    // Find and replace or add the fry subsection
    int array_len = json_object_array_length(fry_array);
    bool found = false;
    
    for (int i = 0; i < array_len; i++) {
        json_object *section = json_object_array_get_idx(fry_array, i);
        json_object *meta_config;
        
        if (json_object_object_get_ex(section, "meta_config", &meta_config)) {
            const char *config_name = json_object_get_string(meta_config);
            if (config_name && strcmp(config_name, fry_service) == 0) {
                // Replace existing element
                json_object_array_put_idx(fry_array, i, service_obj);
                found = true;
                break;
            }
        }
    }
    
    if (!found) {
        // Add new element
        json_object_array_add(fry_array, service_obj);
    }
    
    console_debug(&csl, "Replaced fry section: %s", fry_service);
    return true;
}

// Restart specific services
static int restart_specific_services(const ServiceRestartNeeds *services, bool dev_mode, ConfigApplicationResult *result) {
    if (!services) return -1;
    
    console_info(&csl, "Restarting specific services after rollback");
    
    if (dev_mode) {
        console_info(&csl, "Development mode: simulating service restarts after rollback");
        return 0;
    }
    
    int total_errors = 0;
    char command[256];
    
    if (services->wireless) {
        console_info(&csl, "Restarting wireless after rollback...");
        snprintf(command, sizeof(command), "wifi reload 2>&1");
        int ret = system(command);
        if (WEXITSTATUS(ret) != 0) {
            console_error(&csl, "Failed to restart wireless");
            total_errors++;
        }
        sleep(1);
    }
    
    if (services->opennds) {
        console_info(&csl, "Restarting OpenNDS after rollback...");
        snprintf(command, sizeof(command), "/etc/init.d/opennds restart 2>&1");
        int ret = system(command);
        if (WEXITSTATUS(ret) != 0) {
            console_error(&csl, "Failed to restart OpenNDS");
            total_errors++;
        }
        sleep(1);
    }
    
    if (services->fry_collector) {
        console_info(&csl, "Restarting fry-collector after rollback...");
        snprintf(command, sizeof(command), "/etc/init.d/fry-collector restart 2>&1");
        int ret = system(command);
        if (WEXITSTATUS(ret) != 0) {
            console_error(&csl, "Failed to restart fry-collector");
            total_errors++;
        }
        sleep(1);
    }
    
    if (services->fry_agent) {
        console_info(&csl, "Restarting fry-agent after rollback...");
        snprintf(command, sizeof(command), "/etc/init.d/fry-agent restart 2>&1");
        int ret = system(command);
        if (WEXITSTATUS(ret) != 0) {
            console_error(&csl, "Failed to restart fry-agent");
            total_errors++;
        }
        sleep(1);
    }
    
    if (services->fry_config) {
        console_info(&csl, "Restarting fry-config after rollback...");
        snprintf(command, sizeof(command), "/etc/init.d/fry-config reload 2>&1");
        int ret = system(command);
        if (WEXITSTATUS(ret) != 0) {
            console_error(&csl, "Failed to restart fry-config");
            total_errors++;
        }
    }
    
    return total_errors;
}

// Restore previous hashes after rollback
static void restore_previous_hashes(bool dev_mode) {
    console_info(&csl, "Restoring previous configuration hashes after rollback...");
    
    // Load the previous global hash
    const char *global_hash_file = dev_mode ? 
        "./scripts/hashes/global_config.hash" : 
        "/etc/fry-config/hashes/global_config.hash";
    
    FILE *fp = fopen(global_hash_file, "r");
    if (fp) {
        char previous_hash[32];
        if (fgets(previous_hash, sizeof(previous_hash), fp)) {
            // Remove newline
            char *newline = strchr(previous_hash, '\n');
            if (newline) *newline = '\0';
            console_info(&csl, "Restored global hash: %s", previous_hash);
        }
        fclose(fp);
    } else {
        console_warn(&csl, "No previous global hash found to restore");
    }
    
    // Reset section hashes to force reload from disk on next check
    // This ensures they reflect the actual rollback state
    reset_config_section_hashes();
    
    console_debug(&csl, "Section hashes reset - will reload from disk on next config check");
}

// Restore specific section hashes after granular rollback
static void restore_failed_section_hashes(const ServiceRestartNeeds *failed_services, bool dev_mode) {
    console_info(&csl, "Restoring hashes for rolled-back services...");
    
    const char *hash_dir = dev_mode ? "./scripts/hashes" : "/etc/fry-config/hashes";
    
    if (failed_services->wireless) {
        char wireless_hash_file[512];
        snprintf(wireless_hash_file, sizeof(wireless_hash_file), "%s/wireless.hash", hash_dir);
        console_debug(&csl, "Wireless hash will be reloaded from: %s", wireless_hash_file);
    }
    
    if (failed_services->opennds) {
        char opennds_hash_file[512];
        snprintf(opennds_hash_file, sizeof(opennds_hash_file), "%s/opennds.hash", hash_dir);
        console_debug(&csl, "OpenNDS hash will be reloaded from: %s", opennds_hash_file);
    }
    
    if (failed_services->fry_collector) {
        char collector_hash_file[512];
        snprintf(collector_hash_file, sizeof(collector_hash_file), "%s/fry-collector.hash", hash_dir);
        console_debug(&csl, "Collector hash will be reloaded from: %s", collector_hash_file);
    }
    
    if (failed_services->fry_agent) {
        char agent_hash_file[512];
        snprintf(agent_hash_file, sizeof(agent_hash_file), "%s/fry-agent.hash", hash_dir);
        console_debug(&csl, "Agent hash will be reloaded from: %s", agent_hash_file);
    }
    
    if (failed_services->fry_config) {
        char config_hash_file[512];
        snprintf(config_hash_file, sizeof(config_hash_file), "%s/fry-config.hash", hash_dir);
        console_debug(&csl, "Config hash will be reloaded from: %s", config_hash_file);
    }
    
    // Reset section hashes in memory to force reload from disk
    reset_config_section_hashes();
    
    console_info(&csl, "Section hashes reset - rolled-back services will reload previous hashes");
}

// Execute complete rollback (script failed)
int execute_script_rollback(ConfigApplicationResult *result, bool dev_mode) {
    if (!result) {
        console_error(&csl, "Invalid result parameter for script rollback");
        return -1;
    }
    
    console_warn(&csl, "EXECUTING COMPLETE ROLLBACK (script failed)");
    
    // Load previous successful configuration
    char *successful_config = load_successful_config(dev_mode);
    if (!successful_config) {
        console_error(&csl, "No successful configuration available for rollback");
        strncpy(result->error_message, "No successful configuration available for rollback", 
               sizeof(result->error_message) - 1);
        return -1;
    }
    
    // Apply previous configuration using renderer
    console_info(&csl, "Restoring previous successful configuration...");
    if (apply_config_without_restarts(successful_config, dev_mode) != 0) {
        console_error(&csl, "Failed to restore previous configuration");
        strncpy(result->error_message, "Failed to restore previous configuration", 
               sizeof(result->error_message) - 1);
        free(successful_config);
        return -1;
    }
    
    console_info(&csl, "Previous configuration restored successfully");
    
    // Since script failed, ALL affected services need rollback
    ServiceRestartNeeds all_services = {true, true, true, true, true};
    
    console_info(&csl, "Restarting all services to ensure clean state...");
    int restart_errors = restart_specific_services(&all_services, dev_mode, result);
    
    free(successful_config);
    
    if (restart_errors == 0) {
        console_info(&csl, "COMPLETE ROLLBACK COMPLETED SUCCESSFULLY");
        
        // Restore hashes after rollback
        restore_previous_hashes(dev_mode);
        
        // Update failed services to include _rollback for ALL affected services
        char rollback_services[512] = "";
        char *services_copy = strdup(result->affected_services);
        char *service = strtok(services_copy, ", ");
        bool first = true;
        
        while (service != NULL) {
            if (!first) strcat(rollback_services, ", ");
            strcat(rollback_services, service);
            strcat(rollback_services, "_rollback");
            first = false;
            service = strtok(NULL, ", ");
        }
        free(services_copy);
        
        // Update result
        strncpy(result->failed_services, rollback_services, sizeof(result->failed_services) - 1);
        result->failed_services[sizeof(result->failed_services) - 1] = '\0';
        result->successful_services[0] = '\0'; // No successful services on script failure
        
        return 0;
    } else {
        console_error(&csl, "COMPLETE ROLLBACK COMPLETED WITH ERRORS");
        return -1;
    }
}

// Execute granular rollback (services failed)
int execute_services_rollback(ConfigApplicationResult *result, bool dev_mode) {
    if (!result) {
        console_error(&csl, "Invalid result parameter for services rollback");
        return -1;
    }
    
    console_warn(&csl, "EXECUTING GRANULAR ROLLBACK (services failed)");
    
    // Identify which services failed
    ServiceRestartNeeds failed_services = {false, false, false, false, false};
    
    if (strstr(result->failed_services, "wireless")) {
        failed_services.wireless = true;
    }
    if (strstr(result->failed_services, "opennds")) {
        failed_services.opennds = true;
    }
    if (strstr(result->failed_services, "fry-collector")) {
        failed_services.fry_collector = true;
    }
    if (strstr(result->failed_services, "fry-agent")) {
        failed_services.fry_agent = true;
    }
    if (strstr(result->failed_services, "fry-config")) {
        failed_services.fry_config = true;
    }
    
    console_info(&csl, "Applying rollback configuration for each failed service individually...");
    
    int total_rollback_errors = 0;
    
    // Apply rollback for each failed service individually
    if (failed_services.wireless) {
        console_info(&csl, "Rolling back wireless configuration...");
        char *wireless_section = load_successful_config_section("wireless", NULL, dev_mode);
        if (wireless_section) {
            // Create wrapper JSON for wireless section
            json_object *wrapper = json_object_new_object();
            json_object *device_config = json_object_new_object();
            json_object *wireless_obj = json_tokener_parse(wireless_section);
            
            if (wireless_obj) {
                json_object_object_add(device_config, "wireless", wireless_obj);
                json_object_object_add(wrapper, "device_config", device_config);
                
                const char *wireless_json = json_object_to_json_string(wrapper);
                console_debug(&csl, "Applying wireless rollback JSON (%zu bytes)", strlen(wireless_json));
                
                if (apply_config_without_restarts(wireless_json, dev_mode) != 0) {
                    console_error(&csl, "Failed to apply wireless rollback configuration");
                    total_rollback_errors++;
                }
                
                json_object_put(wrapper);
            } else {
                console_error(&csl, "Failed to parse wireless rollback JSON");
                total_rollback_errors++;
            }
            
            free(wireless_section);
        } else {
            console_error(&csl, "No wireless rollback configuration available");
            total_rollback_errors++;
        }
    }
    
    if (failed_services.opennds) {
        console_info(&csl, "Rolling back OpenNDS configuration...");
        char *opennds_section = load_successful_config_section("opennds", NULL, dev_mode);
        if (opennds_section) {
            // Create wrapper JSON for OpenNDS section
            json_object *wrapper = json_object_new_object();
            json_object *device_config = json_object_new_object();
            json_object *opennds_obj = json_tokener_parse(opennds_section);
            
            if (opennds_obj) {
                json_object_object_add(device_config, "opennds", opennds_obj);
                json_object_object_add(wrapper, "device_config", device_config);
                
                const char *opennds_json = json_object_to_json_string(wrapper);
                console_debug(&csl, "Applying OpenNDS rollback JSON (%zu bytes)", strlen(opennds_json));
                
                if (apply_config_without_restarts(opennds_json, dev_mode) != 0) {
                    console_error(&csl, "Failed to apply OpenNDS rollback configuration");
                    total_rollback_errors++;
                }
                
                json_object_put(wrapper);
            } else {
                console_error(&csl, "Failed to parse OpenNDS rollback JSON");
                total_rollback_errors++;
            }
            
            free(opennds_section);
        } else {
            console_error(&csl, "No OpenNDS rollback configuration available");
            total_rollback_errors++;
        }
    }
    
    if (failed_services.fry_collector) {
        console_info(&csl, "Rolling back fry-collector configuration...");
        char *collector_section = load_successful_config_section("fry", "fry-collector", dev_mode);
        if (collector_section) {
            // Create wrapper JSON for fry-collector section
            json_object *wrapper = json_object_new_object();
            json_object *device_config = json_object_new_object();
            json_object *fry_array = json_object_new_array();
            json_object *collector_obj = json_tokener_parse(collector_section);
            
            if (collector_obj) {
                json_object_array_add(fry_array, collector_obj);
                json_object_object_add(device_config, "fry", fry_array);
                json_object_object_add(wrapper, "device_config", device_config);
                
                const char *collector_json = json_object_to_json_string(wrapper);
                console_debug(&csl, "Applying fry-collector rollback JSON (%zu bytes)", strlen(collector_json));
                
                if (apply_config_without_restarts(collector_json, dev_mode) != 0) {
                    console_error(&csl, "Failed to apply fry-collector rollback configuration");
                    total_rollback_errors++;
                }
                
                json_object_put(wrapper);
            } else {
                console_error(&csl, "Failed to parse fry-collector rollback JSON");
                total_rollback_errors++;
            }
            
            free(collector_section);
        } else {
            console_error(&csl, "No fry-collector rollback configuration available");
            total_rollback_errors++;
        }
    }
    
    if (failed_services.fry_agent) {
        console_info(&csl, "Rolling back fry-agent configuration...");
        char *agent_section = load_successful_config_section("fry", "fry-agent", dev_mode);
        if (agent_section) {
            // Create wrapper JSON for fry-agent section
            json_object *wrapper = json_object_new_object();
            json_object *device_config = json_object_new_object();
            json_object *fry_array = json_object_new_array();
            json_object *agent_obj = json_tokener_parse(agent_section);
            
            if (agent_obj) {
                json_object_array_add(fry_array, agent_obj);
                json_object_object_add(device_config, "fry", fry_array);
                json_object_object_add(wrapper, "device_config", device_config);
                
                const char *agent_json = json_object_to_json_string(wrapper);
                console_debug(&csl, "Applying fry-agent rollback JSON (%zu bytes)", strlen(agent_json));
                
                if (apply_config_without_restarts(agent_json, dev_mode) != 0) {
                    console_error(&csl, "Failed to apply fry-agent rollback configuration");
                    total_rollback_errors++;
                }
                
                json_object_put(wrapper);
            } else {
                console_error(&csl, "Failed to parse fry-agent rollback JSON");
                total_rollback_errors++;
            }
            
            free(agent_section);
        } else {
            console_error(&csl, "No fry-agent rollback configuration available");
            total_rollback_errors++;
        }
    }
    
    if (failed_services.fry_config) {
        console_info(&csl, "Rolling back fry-config configuration...");
        char *config_section = load_successful_config_section("fry", "fry-config", dev_mode);
        if (config_section) {
            // Create wrapper JSON for fry-config section
            json_object *wrapper = json_object_new_object();
            json_object *device_config = json_object_new_object();
            json_object *fry_array = json_object_new_array();
            json_object *config_obj = json_tokener_parse(config_section);
            
            if (config_obj) {
                json_object_array_add(fry_array, config_obj);
                json_object_object_add(device_config, "fry", fry_array);
                json_object_object_add(wrapper, "device_config", device_config);
                
                const char *config_json = json_object_to_json_string(wrapper);
                console_debug(&csl, "Applying fry-config rollback JSON (%zu bytes)", strlen(config_json));
                
                if (apply_config_without_restarts(config_json, dev_mode) != 0) {
                    console_error(&csl, "Failed to apply fry-config rollback configuration");
                    total_rollback_errors++;
                }
                
                json_object_put(wrapper);
            } else {
                console_error(&csl, "Failed to parse fry-config rollback JSON");
                total_rollback_errors++;
            }
            
            free(config_section);
        } else {
            console_error(&csl, "No fry-config rollback configuration available");
            total_rollback_errors++;
        }
    }
    
    if (total_rollback_errors > 0) {
        console_error(&csl, "Failed to apply rollback configuration for %d services", total_rollback_errors);
        strncpy(result->error_message, "Failed to apply rollback configuration", 
               sizeof(result->error_message) - 1);
        return -1;
    }
    
    console_info(&csl, "All rollback configurations applied successfully");
    
    console_info(&csl, "Restarting failed services with rollback configuration...");
    int restart_errors = restart_specific_services(&failed_services, dev_mode, NULL);
    
    if (restart_errors == 0) {
        console_info(&csl, "GRANULAR ROLLBACK COMPLETED SUCCESSFULLY");

        // Restore hashes after rollback
        restore_failed_section_hashes(&failed_services, dev_mode);
        
        // Update failed services to include _rollback
        char rollback_services[512] = "";
        char *services_copy = strdup(result->failed_services);
        char *service = strtok(services_copy, ", ");
        bool first = true;
        
        while (service != NULL) {
            if (!first) strcat(rollback_services, ", ");
            strcat(rollback_services, service);
            strcat(rollback_services, "_rollback");
            first = false;
            service = strtok(NULL, ", ");
        }
        free(services_copy);
        
        // Update result
        strncpy(result->failed_services, rollback_services, sizeof(result->failed_services) - 1);
        result->failed_services[sizeof(result->failed_services) - 1] = '\0';
        
        return 0;
    } else {
        console_error(&csl, "GRANULAR ROLLBACK COMPLETED WITH ERRORS");
        return -1;
    }
}

// Generate rollback report
char* generate_rollback_report(ConfigApplicationResult *result, bool is_script_failure, bool dev_mode) {
    if (!result) return NULL;
    
    json_object *root = json_object_new_object();
    
    // Result status
    const char *result_status = is_script_failure ? "script_failed" : "services_failed";
    json_object *result_obj = json_object_new_string(result_status);
    
    // Standard fields
    json_object *affected_obj = json_object_new_string(result->affected_services);
    json_object *successful_obj = json_object_new_string(result->successful_services);
    json_object *failed_obj = json_object_new_string(result->failed_services);
    
    // Combine errors
    char combined_error[1536] = "";
    if (strlen(result->error_message) > 0) {
        strcat(combined_error, result->error_message);
    }
    if (strlen(result->service_errors) > 0) {
        if (strlen(combined_error) > 0) {
            strcat(combined_error, "; ");
        }
        strcat(combined_error, result->service_errors);
    }
    
    json_object *error_obj = json_object_new_string(combined_error);
    json_object *config_hash_obj = json_object_new_string(result->config_hash);
    
    json_object_object_add(root, "result", result_obj);
    json_object_object_add(root, "affected", affected_obj);
    json_object_object_add(root, "successful", successful_obj);
    json_object_object_add(root, "failed", failed_obj);
    json_object_object_add(root, "error", error_obj);
    json_object_object_add(root, "config_hash", config_hash_obj);
    
    const char *json_string = json_object_to_json_string(root);
    char *result_str = strdup(json_string);
    
    json_object_put(root);
    return result_str;
}