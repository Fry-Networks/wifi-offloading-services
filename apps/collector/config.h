#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdint.h>

// Configuration file paths
#define CONFIG_FILE_OPENWRT "/etc/config/fry-collector"
#define CONFIG_FILE_DEV "./fry-collector.config"
#define CONFIG_FILE_FALLBACK "/tmp/fry-collector.config"

// Default configuration values
#define DEFAULT_ENABLED true
#define DEFAULT_LOGS_ENDPOINT "https://devices.fry.network/logs"
#define DEFAULT_CONSOLE_LOG_LEVEL 1
#define DEFAULT_BATCH_SIZE 50
#define DEFAULT_BATCH_TIMEOUT_MS 10000
#define DEFAULT_QUEUE_SIZE 500
#define DEFAULT_HTTP_TIMEOUT 30
#define DEFAULT_HTTP_RETRIES 2
#define DEFAULT_RECONNECT_DELAY_MS 5000

/**
 * Configuration structure for the collector
 */
typedef struct collector_config {
    // Basic settings
    bool enabled;
    char logs_endpoint[256];
    int console_log_level;

    // Batching configuration
    uint32_t batch_size;
    uint32_t batch_timeout_ms;
    uint32_t queue_size;

    // HTTP configuration
    uint32_t http_timeout;
    uint32_t http_retries;
    uint32_t reconnect_delay_ms;

    // Development settings
    bool dev_mode;

    // Internal state
    bool config_loaded;
    char config_file_path[256];
} collector_config_t;

/**
 * Initialize configuration with default values
 * @param config Pointer to configuration structure
 */
void config_init_defaults(collector_config_t *config);

/**
 * Load configuration from file
 * Tries multiple file paths in order of preference
 * @param config Pointer to configuration structure
 * @return 0 on success, negative error code on failure
 */
int config_load(collector_config_t *config);

/**
 * Load configuration from specific file path
 * @param config Pointer to configuration structure
 * @param file_path Path to configuration file
 * @return 0 on success, negative error code on failure
 */
int config_load_from_file(collector_config_t *config, const char *file_path);

/**
 * Validate configuration values
 * @param config Pointer to configuration structure
 * @return 0 if valid, negative error code if invalid
 */
int config_validate(const collector_config_t *config);

/**
 * Get current configuration
 * @return Pointer to current configuration or NULL if not loaded
 */
const collector_config_t *config_get_current(void);

/**
 * Check if collector is enabled in configuration
 * @return true if enabled, false otherwise
 */
bool config_is_enabled(void);

/**
 * Get logs endpoint URL
 * @return Pointer to endpoint URL string
 */
const char *config_get_logs_endpoint(void);

/**
 * Get batch size
 * @return Configured batch size
 */
uint32_t config_get_batch_size(void);

/**
 * Get batch timeout in milliseconds
 * @return Configured batch timeout
 */
uint32_t config_get_batch_timeout_ms(void);

/**
 * Get queue size
 * @return Configured queue size
 */
uint32_t config_get_queue_size(void);

/**
 * Get HTTP timeout in seconds
 * @return Configured HTTP timeout
 */
uint32_t config_get_http_timeout(void);

/**
 * Get HTTP retry count
 * @return Configured retry count
 */
uint32_t config_get_http_retries(void);

/**
 * Print current configuration (for debugging)
 */
void config_print_current(void);

/**
 * Cleanup configuration resources
 */
void config_cleanup(void);

#endif // CONFIG_H
