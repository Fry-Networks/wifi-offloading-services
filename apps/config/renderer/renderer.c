#include "renderer.h"
#include "core/console.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <json-c/json.h>

static Console csl = {.topic = "renderer"};

//PATHS FOR GRANULAR HASHES
#define DEV_HASH_DIR "./scripts/dev/hashes"
#define PROD_HASH_DIR "/etc/fry-config/hashes"

#define WIRELESS_HASH_FILE "wireless.hash"
#define AGENT_HASH_FILE "fry-agent.hash"
#define COLLECTOR_HASH_FILE "fry-collector.hash"
#define CONFIG_HASH_FILE "fry-config.hash"
#define OPENNDS_HASH_FILE "opennds.hash"

//VARIABLES FOR GRANULAR HASH IN MEMORY
static unsigned long last_wireless_hash = 0;
static unsigned long last_agent_hash = 0;
static unsigned long last_collector_hash = 0;
static unsigned long last_opennds_hash = 0;
static unsigned long last_config_hash = 0;

//FLAGS TO INDICATE IF WE ALREADY LOADED 
static bool wireless_hash_loaded = false;
static bool agent_hash_loaded = false;
static bool collector_hash_loaded = false;
static bool config_hash_loaded = false;
static bool opennds_hash_loaded = false;

// GLOBAL DEVELOPMENT MODE FLAG**
static bool global_dev_mode = false;

// **UNIFIED SCRIPT PATHS - SINGLE renderer_applier.uc**
#define DEV_CONFIG_FILE "./scripts/fry_config.json"
#define DEV_RENDERER_SCRIPT "./scripts/renderer_applier.uc"
#define DEV_UCODE_PATH "/usr/local/bin/ucode"

#define OPENWRT_CONFIG_FILE "/tmp/fry_config.json"
#define OPENWRT_RENDERER_SCRIPT "/etc/fry-config/scripts/renderer_applier.uc"
#define OPENWRT_UCODE_PATH "/usr/bin/ucode"


// Get hash directory path based on mode
static const char *get_hash_dir_path(bool dev_mode) {
    return dev_mode ? DEV_HASH_DIR : PROD_HASH_DIR;
}

// Get full path for hash file
static char *get_hash_file_path(bool dev_mode, const char *hash_filename) {
    const char *hash_dir = get_hash_dir_path(dev_mode);
    
    char *full_path = malloc(512);
    if (full_path) {
        snprintf(full_path, 512, "%s/%s", hash_dir, hash_filename);
    }
    return full_path;
}

//Load hash from disk file, return 0 if not found
static unsigned long load_hash_from_disk(bool dev_mode, const char *hash_filename) {
    char *hash_file = get_hash_file_path(dev_mode, hash_filename);
    if (!hash_file) return 0;
    
    FILE *fp = fopen(hash_file, "r");
    if (!fp) {
        console_debug(&csl, "No previous hash file found at %s", hash_file);
        free(hash_file);
        return 0;
    }
    
    unsigned long hash = 0;
    if (fscanf(fp, "%lu", &hash) == 1) {
        console_debug(&csl, "Loaded hash %lu from %s", hash, hash_file);
    } else {
        console_warn(&csl, "Failed to read hash from %s", hash_file);
        hash = 0;
    }
    
    fclose(fp);
    free(hash_file);
    return hash;
}

// Save hash to disk file with appropriate permissions
static void save_hash_to_disk(bool dev_mode, const char *hash_filename, unsigned long hash) {
    char *hash_file = get_hash_file_path(dev_mode, hash_filename);
    if (!hash_file) return;
    
    FILE *fp = fopen(hash_file, "w");
    if (fp) {
        fprintf(fp, "%lu\n", hash);
        fclose(fp);
        
        // Only change permissions in production mode
        if (!dev_mode) {
            chmod(hash_file, 0644);
        }
        
        console_debug(&csl, "Saved hash %lu to %s", hash, hash_file);
    } else {
        console_warn(&csl, "Failed to save hash to %s (ensure directory exists)", hash_file);
    }
    
    free(hash_file);
}

// Calculate DJB2 hash for string content
static unsigned long calculate_djb2_hash(const char *str) {
    if (!str) return 0;
    
    unsigned long hash = 5381;
    int c;
    
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    }
    
    return hash;
}

// Extract specific configuration section from JSON
static char* extract_config_section(const char *json_config, const char *section_type, const char *meta_config_name) {
    if (!json_config) return NULL;

    json_object *root = json_tokener_parse(json_config);
    if (!root) {
        console_warn(&csl, "Failed to parse JSON for section extraction");
        return NULL;
    }

    json_object *device_config = NULL;
    if (!json_object_object_get_ex(root, "device_config", &device_config)) {
        json_object_put(root);
        return NULL;
    }

    char *section_json = NULL;

    if (strcmp(section_type, "wireless") == 0) {
        // Extract complete wireless section
        json_object *wireless_array = NULL;
        if (json_object_object_get_ex(device_config, "wireless", &wireless_array)) {
            section_json = strdup(json_object_to_json_string(wireless_array));
        }
    } else if (strcmp(section_type, "opennds") == 0) { 
        // Extract complete opennds section
        json_object *opennds_array = NULL;
        if (json_object_object_get_ex(device_config, "opennds", &opennds_array)) {
            section_json = strdup(json_object_to_json_string(opennds_array));
        }
    } else if (strcmp(section_type, "fry") == 0) {
        // Extract specific fry section by meta_config
        json_object *fry_array = NULL;
        if (json_object_object_get_ex(device_config, "fry", &fry_array)) {
            int array_len = json_object_array_length(fry_array);
            
            for (int i = 0; i < array_len; i++) {
                json_object *section = json_object_array_get_idx(fry_array, i);
                json_object *meta_config = NULL;
                
                if (json_object_object_get_ex(section, "meta_config", &meta_config)) {
                    const char *config_name = json_object_get_string(meta_config);
                    if (config_name && strcmp(config_name, meta_config_name) == 0) {
                        section_json = strdup(json_object_to_json_string(section));
                        break;
                    }
                }
            }
        }
    }

    json_object_put(root);
    return section_json;
}

static bool check_section_changed(const char *json_config, const char *section_type,
                                                 const char *meta_config_name, const char *hash_filename,
                                                 unsigned long *last_hash, bool *hash_loaded, bool dev_mode) {
    // Load hash from disk if first time
    if (!*hash_loaded) {
        *last_hash = load_hash_from_disk(dev_mode, hash_filename);
        *hash_loaded = true;
        
        if (*last_hash == 0) {
            console_debug(&csl, "Initializing %s hash tracking", 
                         meta_config_name ? meta_config_name : section_type);
        }
    }
    
    // Calculate current hash
    char *section_json = extract_config_section(json_config, section_type, meta_config_name);
    if (!section_json) {
        console_debug(&csl, "No section found for %s:%s", section_type, 
                     meta_config_name ? meta_config_name : "all");
        return false;
    }
    
    unsigned long current_hash = calculate_djb2_hash(section_json);
    free(section_json);
    
    bool changed = (current_hash != *last_hash);
    
    if (changed) {
        console_debug(&csl, "%s config changed: hash %lu -> %lu (not saved yet)", 
                     meta_config_name ? meta_config_name : section_type, 
                     *last_hash, current_hash);
    } else {
        console_debug(&csl, "%s config unchanged: hash %lu", 
                     meta_config_name ? meta_config_name : section_type, 
                     current_hash);
    }
    
    return changed;
}

static void save_section_hash_after_success(const char *json_config, const char *section_type,
                                           const char *meta_config_name, const char *hash_filename,
                                           unsigned long *last_hash, bool dev_mode) {
    char *section_json = extract_config_section(json_config, section_type, meta_config_name);
    if (!section_json) return;
    
    unsigned long current_hash = calculate_djb2_hash(section_json);
    free(section_json);
    
    *last_hash = current_hash;
    save_hash_to_disk(dev_mode, hash_filename, current_hash);
    
    console_debug(&csl, "%s hash saved after successful config: %lu", 
                 meta_config_name ? meta_config_name : section_type, current_hash);
}

// Check if wireless configuration changed
bool config_affects_wireless(const char *json_config, bool dev_mode) {
    return check_section_changed(json_config, "wireless", NULL, WIRELESS_HASH_FILE,
                                &last_wireless_hash, &wireless_hash_loaded, dev_mode);
}

// Check if fry-agent configuration changed
bool config_affects_fry_agent(const char *json_config, bool dev_mode) {
    return check_section_changed(json_config, "fry", "fry-agent", AGENT_HASH_FILE,
                                &last_agent_hash, &agent_hash_loaded, dev_mode);
}

// Check if fry-collector configuration changed
bool config_affects_fry_collector(const char *json_config, bool dev_mode) {
    return check_section_changed(json_config, "fry", "fry-collector", COLLECTOR_HASH_FILE,
                                &last_collector_hash, &collector_hash_loaded, dev_mode);
}

// Check if fry-config configuration changed
bool config_affects_fry_config(const char *json_config, bool dev_mode) {
    return check_section_changed(json_config, "fry", "fry-config", CONFIG_HASH_FILE,
                                &last_config_hash, &config_hash_loaded, dev_mode);
}

// Check if OpenNDS configuration changed
bool config_affects_opennds(const char *json_config, bool dev_mode) {
    return check_section_changed(json_config, "opennds", NULL, OPENNDS_HASH_FILE,
                                &last_opennds_hash, &opennds_hash_loaded, dev_mode);
}

// Set development mode for renderer
void set_renderer_dev_mode(bool dev_mode) {
    global_dev_mode = dev_mode;
    console_debug(&csl, "Setting renderer dev_mode to: %s", dev_mode ? "true" : "false");
}

// Reset section hashes in memory only
void reset_config_section_hashes(void) {
    console_debug(&csl, "Resetting all section hashes (memory only)");
    last_wireless_hash = 0;
    last_agent_hash = 0;
    last_collector_hash = 0;
    last_config_hash = 0;
    last_opennds_hash = 0;  
    
    // Mark as not loaded to force reload from disk
    wireless_hash_loaded = false;
    agent_hash_loaded = false;
    collector_hash_loaded = false;
    config_hash_loaded = false;
    opennds_hash_loaded = false;
}

// Clear all section hashes from disk and memory
void clear_all_section_hashes(bool dev_mode) {
    console_info(&csl, "Clearing all section hashes from disk and memory");
    
    const char *hash_files[] = {
        WIRELESS_HASH_FILE,
        AGENT_HASH_FILE,
        COLLECTOR_HASH_FILE,
        CONFIG_HASH_FILE,
        OPENNDS_HASH_FILE 
    };
    
    for (int i = 0; i < 5; i++) { 
        char *hash_file = get_hash_file_path(dev_mode, hash_files[i]);
        if (hash_file) {
            if (unlink(hash_file) == 0) {
                console_debug(&csl, "Deleted hash file: %s", hash_file);
            }
            free(hash_file);
        }
    }
    
    // Also clear memory
    reset_config_section_hashes();
}

// Write JSON configuration to file
static int write_config_file(const char *json_config, const char *filepath) {
    FILE *fp = fopen(filepath, "w");
    if (!fp) {
        console_error(&csl, "Failed to create config file: %s", filepath);
        return -1;
    }

    fprintf(fp, "%s", json_config);
    fclose(fp);
    return 0;
}

// Log script output with appropriate log levels
static void log_script_output(const char *output) {
    if (!output) return;
    
    char *output_copy = strdup(output);
    if (!output_copy) return;
    
    char *line = strtok(output_copy, "\n");
    while (line != NULL) {
        // Skip leading whitespace
        while (*line == ' ' || *line == '\t') line++;
        
        if (strlen(line) > 0) {
            if (strstr(line, "Error") || strstr(line, "error") || strstr(line, "ERROR")) {
                console_error(&csl, "Script: %s", line);
            } else if (strstr(line, "Warning") || strstr(line, "warning") || strstr(line, "WARN")) {
                console_warn(&csl, "Script: %s", line);
            } else if (strstr(line, "#") && strlen(line) > 1) {
                console_debug(&csl, "Script: %s", line);
            } else {
                console_info(&csl, "Script: %s", line);
            }
        }
        line = strtok(NULL, "\n");
    }
    
    free(output_copy);
}

// Execute the unified renderer script
static int run_renderer_script(const char *script_path, const char *config_file, 
                              const char *ucode_path, bool dev_mode, bool restart_services) {
    // Verify ucode exists
    if (access(ucode_path, X_OK) != 0) {
        console_error(&csl, "ucode not found at %s", ucode_path);
        return -1;
    }

    // Prepare command with restart_services parameter
    char command[512];
    snprintf(command, sizeof(command), "%s %s %s %s 2>&1", 
             ucode_path, script_path, config_file, restart_services ? "restart" : "no_restart");

    console_info(&csl, "Running renderer in %s mode", dev_mode ? "development" : "OpenWrt");
    console_debug(&csl, "Command: %s", command);

    // Execute script
    FILE *fp = popen(command, "r");
    if (!fp) {
        console_error(&csl, "Failed to execute renderer script");
        return -1;
    }

    // Read output
    char *result = NULL;
    size_t result_size = 0;
    char buffer[1024];
    
    while (fgets(buffer, sizeof(buffer), fp)) {
        size_t buffer_len = strlen(buffer);
        result = realloc(result, result_size + buffer_len + 1);
        if (!result) {
            console_error(&csl, "Memory allocation failed");
            pclose(fp);
            return -1;
        }
        
        if (result_size == 0) {
            strcpy(result, buffer);
        } else {
            strcat(result, buffer);
        }
        result_size += buffer_len;
    }

    int exit_code = pclose(fp);
    int script_exit_code = WEXITSTATUS(exit_code);

    if (result) {
        console_info(&csl, "Renderer script output:");
        log_script_output(result);
        free(result);
    }

    // Clean up config file in production
    if (!dev_mode) {
        unlink(config_file);
    }

    if (script_exit_code != 0) {
        console_error(&csl, "Renderer script failed with exit code %d", script_exit_code);
        return -1;
    }

    return 0;
}

// Apply configuration without service restarts (fry-config manages restarts)
int apply_config_without_restarts(const char *json_config, bool dev_mode) {
    if (!json_config) {
        console_error(&csl, "Invalid JSON config");
        return -1;
    }

    const char *config_file = dev_mode ? DEV_CONFIG_FILE : OPENWRT_CONFIG_FILE;
    const char *renderer_script = dev_mode ? DEV_RENDERER_SCRIPT : OPENWRT_RENDERER_SCRIPT;
    const char *ucode_path = dev_mode ? DEV_UCODE_PATH : OPENWRT_UCODE_PATH;

    // Write JSON to config file
    if (write_config_file(json_config, config_file) != 0) {
        return -1;
    }

    int result = run_renderer_script(renderer_script, config_file, ucode_path, dev_mode, false);
    
    if (result == 0) {
        console_info(&csl, "Configuration rendering completed successfully (no restarts)");
    }
    
    return result;
}

void save_wireless_hash_after_success(const char *json_config, bool dev_mode) {
    save_section_hash_after_success(json_config, "wireless", NULL, WIRELESS_HASH_FILE,
                                   &last_wireless_hash, dev_mode);
}

void save_fry_agent_hash_after_success(const char *json_config, bool dev_mode) {
    save_section_hash_after_success(json_config, "fry", "fry-agent", AGENT_HASH_FILE,
                                   &last_agent_hash, dev_mode);
}

void save_fry_collector_hash_after_success(const char *json_config, bool dev_mode) {
    save_section_hash_after_success(json_config, "fry", "fry-collector", COLLECTOR_HASH_FILE,
                                   &last_collector_hash, dev_mode);
}

void save_fry_config_hash_after_success(const char *json_config, bool dev_mode) {
    save_section_hash_after_success(json_config, "fry", "fry-config", CONFIG_HASH_FILE,
                                   &last_config_hash, dev_mode);
}

void save_opennds_hash_after_success(const char *json_config, bool dev_mode) {
    save_section_hash_after_success(json_config, "opennds", NULL, OPENNDS_HASH_FILE,
                                   &last_opennds_hash, dev_mode);
}