#include "config.h"
#include "core/console.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Console csl = {
    .topic = "config",
};

// Global configuration instance
static collector_config_t g_config;
static bool g_config_initialized = false;

/**
 * Trim whitespace from string
 */
static void trim_whitespace(char *str) {
    char *start = str;
    char *end;

    // Trim leading whitespace
    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') {
        start++;
    }

    // If string is all whitespace
    if (*start == 0) {
        *str = 0;
        return;
    }

    // Trim trailing whitespace
    end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        end--;
    }

    // Write new null terminator
    end[1] = '\0';

    // Move trimmed string to beginning
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

/**
 * Parse boolean value from string
 */
static bool parse_bool(const char *value) {
    if (!value) return false;

    return (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0 ||
            strcasecmp(value, "on") == 0);
}

/**
 * Parse unsigned integer value from string
 */
static uint32_t parse_uint32(const char *value, uint32_t default_value) {
    if (!value) return default_value;

    char *endptr;
    unsigned long parsed = strtoul(value, &endptr, 10);

    if (*endptr != '\0' || parsed > UINT32_MAX) {
        return default_value;
    }

    return (uint32_t)parsed;
}

/**
 * Remove quotes from string value
 */
static void remove_quotes(char *str) {
    size_t len = strlen(str);

    if (len >= 2 && str[0] == '\'' && str[len - 1] == '\'') {
        memmove(str, str + 1, len - 2);
        str[len - 2] = '\0';
    } else if (len >= 2 && str[0] == '"' && str[len - 1] == '"') {
        memmove(str, str + 1, len - 2);
        str[len - 2] = '\0';
    }
}

/**
 * Parse a single configuration option line
 */
static int parse_config_option(collector_config_t *config, const char *line) {
    char line_copy[512];
    strncpy(line_copy, line, sizeof(line_copy) - 1);
    line_copy[sizeof(line_copy) - 1] = '\0';

    // Trim whitespace
    trim_whitespace(line_copy);

    // Skip empty lines and comments
    if (line_copy[0] == '\0' || line_copy[0] == '#') {
        return 0;
    }

    // Look for "option" keyword
    if (strncmp(line_copy, "option", 6) != 0) {
        return 0; // Not an option line
    }

    // Find the option name and value
    char *token = strtok(line_copy + 6, " \t");
    if (!token) return 0;

    char option_name[64];
    strncpy(option_name, token, sizeof(option_name) - 1);
    option_name[sizeof(option_name) - 1] = '\0';

    token = strtok(NULL, "");
    if (!token) return 0;

    char option_value[256];
    strncpy(option_value, token, sizeof(option_value) - 1);
    option_value[sizeof(option_value) - 1] = '\0';
    trim_whitespace(option_value);
    remove_quotes(option_value);

    // Parse specific options
    if (strcmp(option_name, "enabled") == 0) {
        config->enabled = parse_bool(option_value);
        console_debug(&csl, "Parsed enabled: %s", config->enabled ? "true" : "false");
    } else if (strcmp(option_name, "logs_endpoint") == 0) {
        strncpy(config->logs_endpoint, option_value, sizeof(config->logs_endpoint) - 1);
        config->logs_endpoint[sizeof(config->logs_endpoint) - 1] = '\0';
        console_debug(&csl, "Parsed logs_endpoint: %s", config->logs_endpoint);
    } else if (strcmp(option_name, "batch_size") == 0) {
        config->batch_size = parse_uint32(option_value, DEFAULT_BATCH_SIZE);
        console_debug(&csl, "Parsed batch_size: %u", config->batch_size);
    } else if (strcmp(option_name, "batch_timeout_ms") == 0) {
        config->batch_timeout_ms = parse_uint32(option_value, DEFAULT_BATCH_TIMEOUT_MS);
        console_debug(&csl, "Parsed batch_timeout_ms: %u", config->batch_timeout_ms);
    } else if (strcmp(option_name, "queue_size") == 0) {
        config->queue_size = parse_uint32(option_value, DEFAULT_QUEUE_SIZE);
        console_debug(&csl, "Parsed queue_size: %u", config->queue_size);
    } else if (strcmp(option_name, "http_timeout") == 0) {
        config->http_timeout = parse_uint32(option_value, DEFAULT_HTTP_TIMEOUT);
        console_debug(&csl, "Parsed http_timeout: %u", config->http_timeout);
    } else if (strcmp(option_name, "http_retries") == 0) {
        config->http_retries = parse_uint32(option_value, DEFAULT_HTTP_RETRIES);
        console_debug(&csl, "Parsed http_retries: %u", config->http_retries);
    } else if (strcmp(option_name, "reconnect_delay_ms") == 0) {
        config->reconnect_delay_ms = parse_uint32(option_value, DEFAULT_RECONNECT_DELAY_MS);
        console_debug(&csl, "Parsed reconnect_delay_ms: %u", config->reconnect_delay_ms);
    } else if (strcmp(option_name, "dev_mode") == 0) {
        config->dev_mode = parse_bool(option_value);
        console_debug(&csl, "Parsed dev_mode: %s", config->dev_mode ? "true" : "false");
    } else if (strcmp(option_name, "console_log_level") == 0) {
        config->console_log_level = parse_uint32(option_value, DEFAULT_CONSOLE_LOG_LEVEL);
        console_debug(&csl, "Parsed console_log_level: %u", config->console_log_level);
    } else {
        console_debug(&csl, "Unknown configuration option: %s", option_name);
    }

    return 0;
}

void config_init_defaults(collector_config_t *config) {
    memset(config, 0, sizeof(collector_config_t));

    config->enabled = DEFAULT_ENABLED;
    strncpy(config->logs_endpoint, DEFAULT_LOGS_ENDPOINT, sizeof(config->logs_endpoint) - 1);
    config->logs_endpoint[sizeof(config->logs_endpoint) - 1] = '\0';

    config->batch_size = DEFAULT_BATCH_SIZE;
    config->batch_timeout_ms = DEFAULT_BATCH_TIMEOUT_MS;
    config->queue_size = DEFAULT_QUEUE_SIZE;

    config->http_timeout = DEFAULT_HTTP_TIMEOUT;
    config->http_retries = DEFAULT_HTTP_RETRIES;
    config->reconnect_delay_ms = DEFAULT_RECONNECT_DELAY_MS;

    config->dev_mode = false;
    config->console_log_level = DEFAULT_CONSOLE_LOG_LEVEL;

    config->config_loaded = false;
    config->config_file_path[0] = '\0';

    console_debug(&csl, "Configuration initialized with defaults");
}

int config_load_from_file(collector_config_t *config, const char *file_path) {
    FILE *file;
    char line[512];
    int line_number = 0;
    bool in_collector_section = false;

    if (!config || !file_path) {
        return -EINVAL;
    }

    console_debug(&csl, "Attempting to load configuration from: %s", file_path);

    file = fopen(file_path, "r");
    if (!file) {
        console_debug(&csl, "Could not open config file %s: %s", file_path, strerror(errno));
        return -errno;
    }

    while (fgets(line, sizeof(line), file)) {
        line_number++;
        trim_whitespace(line);

        // Skip empty lines and comments
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }

        // Check for section header
        if (strncmp(line, "config fry_collector", 22) == 0) {
            in_collector_section = true;
            console_debug(&csl, "Found fry_collector section at line %d", line_number);
            continue;
        }

        // Check for new section (end of our section)
        if (strncmp(line, "config ", 7) == 0 && strncmp(line, "config fry_collector", 22) != 0) {
            in_collector_section = false;
            continue;
        }

        // Parse options only if we're in the collector section
        if (in_collector_section) {
            int ret = parse_config_option(config, line);
            if (ret < 0) {
                console_warn(&csl, "Error parsing line %d: %s", line_number, line);
            }
        }
    }

    fclose(file);

    // Store the config file path
    strncpy(config->config_file_path, file_path, sizeof(config->config_file_path) - 1);
    config->config_file_path[sizeof(config->config_file_path) - 1] = '\0';
    config->config_loaded = true;

    console_info(&csl, "Configuration loaded from: %s", file_path);
    return 0;
}

int config_load(collector_config_t *config) {
    const char *config_paths[] = {CONFIG_FILE_OPENWRT, CONFIG_FILE_DEV, CONFIG_FILE_FALLBACK, NULL};

    if (!config) {
        return -EINVAL;
    }

    // Initialize with defaults first
    config_init_defaults(config);

    // Try to load from each path in order
    for (int i = 0; config_paths[i] != NULL; i++) {
        if (access(config_paths[i], R_OK) == 0) {
            int ret = config_load_from_file(config, config_paths[i]);
            if (ret == 0) {
                console_info(&csl, "Successfully loaded configuration from: %s", config_paths[i]);
                return 0;
            }
        }
    }

    console_warn(&csl, "No configuration file found, using defaults");
    config->config_loaded = false;
    return -ENOENT;
}

int config_validate(const collector_config_t *config) {
    if (!config) {
        return -EINVAL;
    }

    // Validate logs endpoint
    if (strlen(config->logs_endpoint) == 0) {
        console_error(&csl, "Invalid configuration: logs_endpoint is empty");
        return -EINVAL;
    }

    if (strncmp(config->logs_endpoint, "http://", 7) != 0 && strncmp(config->logs_endpoint, "https://", 8) != 0) {
        console_error(&csl, "Invalid configuration: logs_endpoint must start with http:// or https://");
        return -EINVAL;
    }

    // Validate batch size
    if (config->batch_size == 0 || config->batch_size > 1000) {
        console_error(&csl, "Invalid configuration: batch_size must be between 1 and 1000");
        return -EINVAL;
    }

    // Validate queue size
    if (config->queue_size == 0 || config->queue_size > 10000) {
        console_error(&csl, "Invalid configuration: queue_size must be between 1 and 10000");
        return -EINVAL;
    }

    // Validate batch timeout
    if (config->batch_timeout_ms < 1000 || config->batch_timeout_ms > 300000) {
        console_error(&csl, "Invalid configuration: batch_timeout_ms must be between 1000 and 300000");
        return -EINVAL;
    }

    // Validate HTTP timeout
    if (config->http_timeout == 0 || config->http_timeout > 300) {
        console_error(&csl, "Invalid configuration: http_timeout must be between 1 and 300 seconds");
        return -EINVAL;
    }

    console_debug(&csl, "Configuration validation passed");
    return 0;
}

const collector_config_t *config_get_current(void) {
    if (!g_config_initialized) {
        // Lazy initialization
        config_load(&g_config);
        g_config_initialized = true;
    }

    return &g_config;
}

bool config_is_enabled(void) {
    const collector_config_t *config = config_get_current();
    return config ? config->enabled : DEFAULT_ENABLED;
}

const char *config_get_logs_endpoint(void) {
    const collector_config_t *config = config_get_current();
    return config ? config->logs_endpoint : DEFAULT_LOGS_ENDPOINT;
}

uint32_t config_get_batch_size(void) {
    const collector_config_t *config = config_get_current();
    return config ? config->batch_size : DEFAULT_BATCH_SIZE;
}

uint32_t config_get_batch_timeout_ms(void) {
    const collector_config_t *config = config_get_current();
    return config ? config->batch_timeout_ms : DEFAULT_BATCH_TIMEOUT_MS;
}

uint32_t config_get_queue_size(void) {
    const collector_config_t *config = config_get_current();
    return config ? config->queue_size : DEFAULT_QUEUE_SIZE;
}

uint32_t config_get_http_timeout(void) {
    const collector_config_t *config = config_get_current();
    return config ? config->http_timeout : DEFAULT_HTTP_TIMEOUT;
}

uint32_t config_get_http_retries(void) {
    const collector_config_t *config = config_get_current();
    return config ? config->http_retries : DEFAULT_HTTP_RETRIES;
}

void config_print_current(void) {
    const collector_config_t *config = config_get_current();

    if (!config) {
        console_info(&csl, "No configuration loaded");
        return;
    }

    console_info(&csl, "Current Configuration:");
    console_info(&csl, "  enabled: %s", config->enabled ? "true" : "false");
    console_info(&csl, "  logs_endpoint: %s", config->logs_endpoint);
    console_info(&csl, "  batch_size: %u", config->batch_size);
    console_info(&csl, "  batch_timeout_ms: %u", config->batch_timeout_ms);
    console_info(&csl, "  queue_size: %u", config->queue_size);
    console_info(&csl, "  http_timeout: %u", config->http_timeout);
    console_info(&csl, "  http_retries: %u", config->http_retries);
    console_info(&csl, "  reconnect_delay_ms: %u", config->reconnect_delay_ms);
    console_info(&csl, "  dev_mode: %s", config->dev_mode ? "true" : "false");
    console_info(&csl, "  console_log_level: %u", config->console_log_level);

    if (config->config_loaded) {
        console_info(&csl, "  config_file: %s", config->config_file_path);
    } else {
        console_info(&csl, "  config_file: (using defaults)");
    }
}

void config_cleanup(void) {
    g_config_initialized = false;
    memset(&g_config, 0, sizeof(g_config));
    console_debug(&csl, "Configuration cleanup complete");
}
