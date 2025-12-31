#include "config.h"
#include "core/console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

static Console csl = {
    .topic = "config",
};

static remote_config_t g_config;
static bool g_config_initialized = false;

static void trim_whitespace(char *str) {
    char *start = str;
    char *end;

    while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') {
        start++;
    }

    if (*start == 0) {
        *str = 0;
        return;
    }

    end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) {
        end--;
    }

    end[1] = '\0';

    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

static void remove_quotes(char *str) {
    size_t len = strlen(str);
    if (len >= 2 && ((str[0] == '"' && str[len-1] == '"') || (str[0] == '\'' && str[len-1] == '\''))) {
        memmove(str, str + 1, len - 2);
        str[len - 2] = '\0';
    }
}

static int parse_config_option(remote_config_t *config, const char *line) {
    char line_copy[512];
    strncpy(line_copy, line, sizeof(line_copy) - 1);
    line_copy[sizeof(line_copy) - 1] = '\0';

    trim_whitespace(line_copy);
    if (line_copy[0] == '\0' || line_copy[0] == '#') return 0;

    if (strncmp(line_copy, "option", 6) != 0) return 0;

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

    if (strcmp(option_name, "config_endpoint") == 0) {
        strncpy(config->config_endpoint, option_value, sizeof(config->config_endpoint) - 1);
        config->config_endpoint[sizeof(config->config_endpoint) - 1] = '\0';
        console_debug(&csl, "Parsed config_endpoint: %s", config->config_endpoint);
    } else if (strcmp(option_name, "enabled") == 0) {
        config->enabled = (strcmp(option_value, "1") == 0 || strcasecmp(option_value, "true") == 0);
        console_debug(&csl, "Parsed enabled: %s", config->enabled ? "true" : "false");
    } else if (strcmp(option_name, "console_log_level") == 0) {
        config->console_log_level = atoi(option_value);
        if (config->console_log_level < 0) config->console_log_level = 0;
        if (config->console_log_level > 7) config->console_log_level = 7;
        console_debug(&csl, "Parsed console_log_level: %d", config->console_log_level);
    } else if (strcmp(option_name, "config_interval") == 0) { 
        config->config_interval_ms = (uint32_t)atoi(option_value);
        console_debug(&csl, "Parsed config_interval: %u ms", config->config_interval_ms);
    } else {
        console_debug(&csl, "Unknown configuration option: %s", option_name);
    }

    return 0;
}

void config_init_defaults(remote_config_t *config) {
    memset(config, 0, sizeof(remote_config_t));
    strncpy(config->config_endpoint, DEFAULT_CONFIG_ENDPOINT, sizeof(config->config_endpoint) - 1);
    config->config_endpoint[sizeof(config->config_endpoint) - 1] = '\0';
    config->enabled = DEFAULT_ENABLED;
    config->console_log_level = DEFAULT_CONSOLE_LOG_LEVEL;
    config->config_interval_ms = DEFAULT_CONFIG_INTERVAL_MS;
    config->config_loaded = false;
    config->config_file_path[0] = '\0';
    console_debug(&csl, "Configuration initialized with defaults");
}

int config_load_from_file(remote_config_t *config, const char *file_path) {
    FILE *file;
    char line[512];
    int line_number = 0;
    bool in_section = false;

    if (!config || !file_path) return -EINVAL;

    file = fopen(file_path, "r");
    if (!file) {
        console_debug(&csl, "Could not open config file %s: %s", file_path, strerror(errno));
        return -errno;
    }

    while (fgets(line, sizeof(line), file)) {
        line_number++;
        trim_whitespace(line);
        if (line[0] == '\0' || line[0] == '#') continue;

        if (strncmp(line, "config fry_config", 19) == 0) {
            in_section = true;
            continue;
        }

        if (strncmp(line, "config ", 7) == 0 && strncmp(line, "config fry_config", 19) != 0) {
            in_section = false;
            continue;
        }

        if (in_section) {
            parse_config_option(config, line);
        }
    }

    fclose(file);
    strncpy(config->config_file_path, file_path, sizeof(config->config_file_path) - 1);
    config->config_file_path[sizeof(config->config_file_path) - 1] = '\0';
    config->config_loaded = true;
    return 0;
}

int config_load(remote_config_t *config) {
    const char *paths[] = {
        CONFIG_FILE_OPENWRT,
        CONFIG_FILE_DEV,
        CONFIG_FILE_FALLBACK,
        NULL
    };

    config_init_defaults(config);

    for (int i = 0; paths[i]; i++) {
        if (access(paths[i], R_OK) == 0) {
            if (config_load_from_file(config, paths[i]) == 0) {
                console_info(&csl, "Loaded config from %s", paths[i]);
                return 0;
            }
        }
    }

    console_warn(&csl, "No config file found, using defaults");
    config->config_loaded = false;
    return -ENOENT;
}

const remote_config_t* config_get_current(void) {
    if (!g_config_initialized) {
        config_load(&g_config);
        g_config_initialized = true;
    }
    return &g_config;
}

const char* config_get_config_endpoint(void) {
    const remote_config_t *config = config_get_current();
    return config ? config->config_endpoint : DEFAULT_CONFIG_ENDPOINT;
}

bool config_is_enabled(void) {
    const remote_config_t *config = config_get_current();
    return config ? config->enabled : DEFAULT_ENABLED;
}

int config_get_console_log_level(void) {
    const remote_config_t *config = config_get_current();
    return config ? config->console_log_level : DEFAULT_CONSOLE_LOG_LEVEL;
}

uint32_t config_get_config_interval_ms(void) {
    const remote_config_t *config = config_get_current();
    return config ? config->config_interval_ms : DEFAULT_CONFIG_INTERVAL_MS;
}

void config_print_current(void) {
    const remote_config_t *config = config_get_current();
    if (!config) {
        console_info(&csl, "No configuration loaded");
        return;
    }

    console_info(&csl, "Config enabled: %s", config->enabled ? "true" : "false");
    console_info(&csl, "Config endpoint: %s", config->config_endpoint);
    console_info(&csl, "Console log level: %d", config->console_log_level);
    console_info(&csl, "Config interval: %u ms", config->config_interval_ms);
    if (config->config_loaded) {
        console_info(&csl, "Config file: %s", config->config_file_path);
    } else {
        console_info(&csl, "Config file: (using defaults)");
    }
}

void config_cleanup(void) {
    g_config_initialized = false;
    memset(&g_config, 0, sizeof(g_config));
    console_debug(&csl, "Configuration cleanup complete");
}
