#ifndef ROLLBACK_H
#define ROLLBACK_H

#include <stdbool.h>
#include <time.h>

#define MAX_CONFIG_SIZE (2 * 1024 * 1024)  // 2MB limit

// Service restart tracking
typedef struct {
    bool wireless;
    bool fry_agent;
    bool fry_collector;
    bool fry_config;
    bool opennds;
} ServiceRestartNeeds;

// Application result tracking
typedef struct {
    bool script_success;
    bool services_restarted_successfully;
    char affected_services[256];
    char successful_services[256];
    char failed_services[256];
    char error_message[512];
    char service_errors[1024];
    char config_hash[16];
} ConfigApplicationResult;

// Core rollback functions
int save_successful_config(const char *config_json, const char *global_hash, bool dev_mode);
int execute_script_rollback(ConfigApplicationResult *result, bool dev_mode);
int execute_services_rollback(ConfigApplicationResult *result, bool dev_mode);
char* generate_rollback_report(ConfigApplicationResult *result, bool is_script_failure, bool dev_mode);

// Granular config functions
char* extract_config_section(const char *full_config_json, const char *section_type, const char *meta_config_name);
int save_successful_config_section(const char *full_config_json, const char *section_type, 
                                  const char *meta_config_name, const char *section_hash, bool dev_mode);
char* load_successful_config_section(const char *section_type, const char *meta_config_name, bool dev_mode);

#endif // ROLLBACK_H