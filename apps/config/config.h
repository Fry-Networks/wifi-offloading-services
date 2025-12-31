#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define CONFIG_FILE_OPENWRT "/etc/config/fry-config"
#define CONFIG_FILE_DEV "./scripts/fry-config.config"

#define CONFIG_FILE_FALLBACK "/tmp/fry-config.config"
#define DEFAULT_CONFIG_ENDPOINT "https://devices.fry.network/device_config"

#define DEFAULT_ENABLED true
#define DEFAULT_CONSOLE_LOG_LEVEL 7
#define DEFAULT_CONFIG_INTERVAL_MS 900000

typedef struct remote_config {
    char config_endpoint[256]; 
    bool enabled;
    bool config_loaded;
    char config_file_path[256];
    int console_log_level;
    uint32_t config_interval_ms;
} remote_config_t;

void config_init_defaults(remote_config_t *config);
int config_load(remote_config_t *config);
int config_load_from_file(remote_config_t *config, const char *file_path);
const remote_config_t* config_get_current(void);
const char* config_get_config_endpoint(void);
bool config_is_enabled(void);
int config_get_console_log_level(void);
uint32_t config_get_config_interval_ms(void);
void config_print_current(void);
void config_cleanup(void);

#endif // CONFIG_H
