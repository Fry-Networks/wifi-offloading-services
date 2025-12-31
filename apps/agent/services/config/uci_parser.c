#include "uci_parser.h"
#include "config.h"
#include "core/console.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Console csl = {
    .topic = "uci_parser",
};

/**
 * Trim whitespace from the beginning and end of a string
 * @param str String to trim (modified in place)
 * @return Pointer to the trimmed string
 */
static char *trim_whitespace(char *str) {
    char *end;

    // Trim leading space
    while (*str == ' ' || *str == '\t')
        str++;

    if (*str == 0) // All spaces?
        return str;

    // Trim trailing space
    end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
        end--;

    // Write new null terminator
    *(end + 1) = 0;

    return str;
}

/**
 * Remove quotes from a string value (both single and double quotes)
 * @param str String to process (modified in place)
 * @return Pointer to the string without quotes
 */
static char *remove_quotes(char *str) {
    if (str == NULL) return NULL;

    str = trim_whitespace(str);
    int len = strlen(str);

    if (len >= 2 && str[0] == '\'' && str[len - 1] == '\'') {
        str[len - 1] = '\0';
        return str + 1;
    }

    if (len >= 2 && str[0] == '"' && str[len - 1] == '"') {
        str[len - 1] = '\0';
        return str + 1;
    }

    return str;
}

/**
 * Parse a single configuration option and update the config structure
 * @param config Pointer to the config structure to update
 * @param option_name Name of the configuration option
 * @param option_value Value of the configuration option
 */
static void parse_config_option(Config *config, const char *option_name, const char *option_value) {
    if (strcmp(option_name, "enabled") == 0) {
        int enabled = atoi(option_value);
        config->enabled = (enabled != 0);
    } else if (strcmp(option_name, "main_api") == 0) {
        snprintf(config->main_api, sizeof(config->main_api), "%s", option_value);
    } else if (strcmp(option_name, "accounting_api") == 0) {
        snprintf(config->accounting_api, sizeof(config->accounting_api), "%s", option_value);
    } else if (strcmp(option_name, "devices_api") == 0) {
        snprintf(config->devices_api, sizeof(config->devices_api), "%s", option_value);
    } else if (strcmp(option_name, "access_interval") == 0) {
        config->access_interval = atoi(option_value);
    } else if (strcmp(option_name, "device_status_interval") == 0) {
        config->device_status_interval = atoi(option_value);
    } else if (strcmp(option_name, "console_log_level") == 0) {
        int console_log_level = atoi(option_value);
        console_set_level(console_log_level);
    } else if (strcmp(option_name, "monitoring_enabled") == 0) {
        config->monitoring_enabled = (atoi(option_value) == 1);
    } else if (strcmp(option_name, "monitoring_interval") == 0) {
        config->monitoring_interval = atoi(option_value);
    } else if (strcmp(option_name, "monitoring_minimum_interval") == 0) {
        config->monitoring_minimum_interval = atoi(option_value);
    } else if (strcmp(option_name, "monitoring_maximum_interval") == 0) {
        config->monitoring_maximum_interval = atoi(option_value);
    } else if (strcmp(option_name, "speed_test_enabled") == 0) {
        config->speed_test_enabled = (atoi(option_value) == 1);
    } else if (strcmp(option_name, "speed_test_interval") == 0) {
        config->speed_test_interval = atoi(option_value);
    } else if (strcmp(option_name, "speed_test_minimum_interval") == 0) {
        config->speed_test_minimum_interval = atoi(option_value);
    } else if (strcmp(option_name, "speed_test_maximum_interval") == 0) {
        config->speed_test_maximum_interval = atoi(option_value);
    } else if (strcmp(option_name, "speed_test_latency_attempts") == 0) {
        config->speed_test_latency_attempts = atoi(option_value);
    } else if (strcmp(option_name, "device_context_interval") == 0) {
        config->device_context_interval = atoi(option_value);
    } else if (strcmp(option_name, "mqtt_broker_url") == 0) {
        snprintf(config->mqtt_broker_url, sizeof(config->mqtt_broker_url), "%s", option_value);
    } else if (strcmp(option_name, "mqtt_keepalive") == 0) {
        config->mqtt_keepalive = atoi(option_value);
    } else if (strcmp(option_name, "mqtt_task_interval") == 0) {
        config->mqtt_task_interval = atoi(option_value);
    } else if (strcmp(option_name, "reboot_enabled") == 0) {
        config->reboot_enabled = (atoi(option_value) == 1);
    } else if (strcmp(option_name, "reboot_interval") == 0) {
        config->reboot_interval = atoi(option_value);
    } else if (strcmp(option_name, "firmware_update_enabled") == 0) {
        config->firmware_update_enabled = (atoi(option_value) == 1);
    } else if (strcmp(option_name, "firmware_update_interval") == 0) {
        config->firmware_update_interval = atoi(option_value);
    } else if (strcmp(option_name, "use_n_sysupgrade") == 0) {
        config->use_n_sysupgrade = (atoi(option_value) != 0);
    } else if (strcmp(option_name, "package_update_enabled") == 0) {
        config->package_update_enabled = (atoi(option_value) == 1);
    } else if (strcmp(option_name, "package_update_interval") == 0) {
        config->package_update_interval = atoi(option_value);
    } else if (strcmp(option_name, "diagnostic_interval") == 0) {
        config->diagnostic_interval = atoi(option_value);
    } else if (strcmp(option_name, "external_connectivity_host") == 0) {
        snprintf(config->external_connectivity_host, sizeof(config->external_connectivity_host), "%s", option_value);
    } else if (strcmp(option_name, "nds_interval") == 0) {
        config->nds_interval = atoi(option_value);
    } else if (strcmp(option_name, "time_sync_server") == 0) {
        snprintf(config->time_sync_server, sizeof(config->time_sync_server), "%s", option_value);
    } else if (strcmp(option_name, "time_sync_interval") == 0) {
        config->time_sync_interval = atoi(option_value);
    } else if (strcmp(option_name, "collector_enabled") == 0) {
        config->collector_enabled = (atoi(option_value) == 1);
    } else if (strcmp(option_name, "collector_interval") == 0) {
        config->collector_interval = atoi(option_value);
    }
}

bool parse_uci_config(const char *config_path, Config *config) {
    FILE *file = fopen(config_path, "r");
    if (file == NULL) {
        console_error(&csl, "Failed to open config file: %s", config_path);
        return false;
    }

    char line[512];
    bool in_fry_section = false;

    while (fgets(line, sizeof(line), file)) {
        char *trimmed_line = trim_whitespace(line);

        // Skip empty lines and comments
        if (strlen(trimmed_line) == 0 || trimmed_line[0] == '#') {
            continue;
        }

        // Check for section start
        if (strstr(trimmed_line, "config fry_agent") != NULL) {
            in_fry_section = true;
            continue;
        }

        // Check for new section (exit fry section)
        if (strncmp(trimmed_line, "config ", 7) == 0 && strstr(trimmed_line, "fry_agent") == NULL) {
            in_fry_section = false;
            continue;
        }

        // Parse options only if we're in the fry section
        if (in_fry_section && strncmp(trimmed_line, "option ", 7) == 0) {
            char *option_line = trimmed_line + 7; // Skip "option "
            char *space_pos = strchr(option_line, ' ');

            if (space_pos != NULL) {
                *space_pos = '\0';
                char *option_name = option_line;
                char *option_value = remove_quotes(space_pos + 1);

                parse_config_option(config, option_name, option_value);
            }
        }
    }

    fclose(file);
    return true;
}
