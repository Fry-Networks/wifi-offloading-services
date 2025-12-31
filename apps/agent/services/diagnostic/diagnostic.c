#define _DEFAULT_SOURCE

/*
 * Diagnostic Service - Network and API Health Monitoring
 *
 * This service implements a two-tier diagnostic strategy:
 *
 * 1. INITIALIZATION DIAGNOSTICS (comprehensive):
 *    - DNS resolution for ALL critical domains (APIs, MQTT, time sync, external)
 *    - Basic internet connectivity test
 *    - Health checks for ALL Fry APIs (main, accounting, devices)
 *
 * 2. PERIODIC DIAGNOSTICS (selective for performance):
 *    - DNS check for accounting API (most critical domain)
 *    - Internet connectivity test
 *    - Accounting API health check (core functionality)
 *    - Access token validation
 *
 * This approach ensures comprehensive validation at startup while maintaining
 * efficient periodic monitoring suitable for resource-constrained router/AP environments.
 *
 * Critical domains checked:
 * - Main API: Device status reporting
 * - Accounting API: Registration, tokens, device context, firmware updates
 * - Devices API: Package updates
 * - MQTT Broker: Real-time messaging
 * - Time Sync Server: System time synchronization
 * - External hosts: General internet connectivity
 */

#include "diagnostic.h"
#include "core/console.h"
#include "core/retry.h"
#include "core/uloop_scheduler.h"
#include "http/http-requests.h"
#include "services/access_token.h"
#include "services/config/config.h"
#include "services/device_info.h"
#include "services/exit_handler.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// Paths to LED triggers
#define GREEN_LED_TRIGGER "/sys/devices/platform/leds/leds/green:lan/trigger"
#define RED_LED_TRIGGER "/sys/devices/platform/leds/leds/red:wan/trigger"
#define BLUE_LED_TRIGGER "/sys/devices/platform/leds/leds/blue:wlan2g/trigger"
#define BLUE_LED_TRIGGER_ODYSSEY "/sys/devices/platform/leds/leds/blue:wlan/trigger"

static Console csl = {
    .topic = "diagnostic",
};

static DeviceInfo *diagnostic_device_info;

// Utility function to extract domain from URL
static char *extract_domain_from_url(const char *url) {
    if (url == NULL) return NULL;

    // Skip protocol (http:// or https://)
    const char *start = url;
    if (strncmp(url, "http://", 7) == 0) {
        start = url + 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        start = url + 8;
    }

    // Find the end of the domain (first '/' or ':' or end of string)
    const char *end = start;
    while (*end && *end != '/' && *end != ':') {
        end++;
    }

    // Allocate and copy domain
    size_t domain_len = end - start;
    char *domain = malloc(domain_len + 1);
    if (domain) {
        strncpy(domain, start, domain_len);
        domain[domain_len] = '\0';
    }

    return domain;
}

// Network check functions (moved from network_check.c)

// \brief Check if the device has internet connection with a single ping. Validating both IPv4 and IPv6
// \param host Host to ping
static bool ping(void *params) {
    if (params == NULL) return false;
    char *host = (char *)params;

    char command[256];
    snprintf(command, sizeof(command), "ping -6 -c 1 %s > /dev/null 2>&1", host);
    int status = system(command);

    if (status == 0) {
        console_info(&csl, "Ping to %s successful (IPv6)", host);
        return true;
    } else {
        snprintf(command, sizeof(command), "ping -4 -c 1 %s > /dev/null 2>&1", host);
        status = system(command);

        if (status == 0) {
            console_info(&csl, "Ping to %s successful (IPv4)", host);
            return true;
        } else {
            console_error(&csl, "Ping to %s failed (IPv4 and IPv6)", host);
            return false;
        }
    }
}

bool internet_check(const char *host) {
    RetryConfig config;
    config.retry_func = ping;
    config.retry_params = (void *)host;
    config.attempts = 5;
    config.delay_seconds = 30;
    bool result = retry(&config);
    if (result == true) {
        console_info(&csl, "Internet connection is available");
        return true;
    } else {
        console_error(&csl, "No internet connection after %d attempts", config.attempts);
        return false;
    }
}

// \brief Check if the device can reach the fry accounting API via the /health endpoint
static bool fry_health() {
    char url[256];
    snprintf(url, sizeof(url), "%s/health", config.accounting_api);
    console_info(&csl, "Fry health url %s", url);
    HttpGetOptions get_fry_options = {
        .url = url,
        .bearer_token = NULL,
    };
    HttpResult result = http_get(&get_fry_options);

    free(result.response_buffer);

    if (result.is_error) {
        return false;
    } else {
        return true;
    }
}

bool fry_check() {
    RetryConfig config;
    config.retry_func = fry_health;
    config.retry_params = NULL;
    config.attempts = 5;
    config.delay_seconds = 30;
    bool result = retry(&config);
    if (result == true) {
        console_info(&csl, "Fry is reachable");
        return true;
    } else {
        console_error(&csl, "Fry is not reachable after %d attempts ... exiting", config.attempts);
        return false;
    }
}

// Write to LED trigger
static void set_led_trigger(const char *led_path, const char *mode) {
    FILE *fp = fopen(led_path, "w");
    if (fp) {
        fprintf(fp, "%s", mode);
        fclose(fp);
        console_debug(&csl, "Set LED at '%s' to mode '%s'", led_path, mode);

    } else {
        console_error(&csl, "Failed to write to LED at '%s' with mode '%s'", led_path, mode);
    }
}

// DNS resolution check with retry logic
static bool dns_resolve_single_attempt(void *params) {
    if (params == NULL) return false;
    char *host = (char *)params;

    struct addrinfo hints, *result;
    int dns_status;

    // Clear hints structure
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP socket

    console_info(&csl, "Resolving hostname: %s", host);

    dns_status = getaddrinfo(host, NULL, &hints, &result);

    if (dns_status == 0) {
        console_info(&csl, "DNS resolution successful for %s", host);

        // Print first resolved address for debugging
        if (result != NULL) {
            char ip_str[INET6_ADDRSTRLEN];
            void *addr;
            const char *ip_version;

            if (result->ai_family == AF_INET) {
                struct sockaddr_in *ipv4 = (struct sockaddr_in *)result->ai_addr;
                addr = &(ipv4->sin_addr);
                ip_version = "IPv4";
            } else {
                struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)result->ai_addr;
                addr = &(ipv6->sin6_addr);
                ip_version = "IPv6";
            }

            if (inet_ntop(result->ai_family, addr, ip_str, sizeof(ip_str)) != NULL) {
                console_info(&csl, "Resolved %s to %s: %s", host, ip_version, ip_str);
            }
        }

        freeaddrinfo(result);
        return true;
    } else {
        console_error(&csl, "DNS resolution failed for %s: %s", host, gai_strerror(dns_status));
        return false;
    }
}

bool dns_resolve_check(const char *host) {
    RetryConfig config;
    config.retry_func = dns_resolve_single_attempt;
    config.retry_params = (void *)host;
    config.attempts = 3;
    config.delay_seconds = 5;
    bool result = retry(&config);
    if (result == true) {
        console_info(&csl, "DNS resolution successful for %s", host);
        return true;
    } else {
        console_error(&csl, "DNS resolution failed for %s after %d attempts", host, config.attempts);
        return false;
    }
}

// Comprehensive DNS resolution check for all critical domains
bool comprehensive_dns_check() {
    console_info(&csl, "Starting comprehensive DNS resolution checks");

    // List of critical domains to check
    const char *critical_hosts[] = {
        config.mqtt_broker_url,            // MQTT broker
        config.time_sync_server,           // Time sync server
        config.external_connectivity_host, // External internet connectivity test
        NULL                               // Sentinel
    };

    // Extract and check domains from API URLs
    char *main_domain = extract_domain_from_url(config.main_api);
    char *accounting_domain = extract_domain_from_url(config.accounting_api);
    char *devices_domain = extract_domain_from_url(config.devices_api);

    bool all_passed = true;

    // Check API domains
    if (main_domain) {
        console_info(&csl, "Checking main API domain: %s", main_domain);
        if (!dns_resolve_check(main_domain)) {
            all_passed = false;
        }
        free(main_domain);
    }

    if (accounting_domain) {
        console_info(&csl, "Checking accounting API domain: %s", accounting_domain);
        if (!dns_resolve_check(accounting_domain)) {
            all_passed = false;
        }
        free(accounting_domain);
    }

    if (devices_domain) {
        console_info(&csl, "Checking devices API domain: %s", devices_domain);
        if (!dns_resolve_check(devices_domain)) {
            all_passed = false;
        }
        free(devices_domain);
    }

    // Check other critical hosts
    for (int i = 0; critical_hosts[i] != NULL; i++) {
        console_info(&csl, "Checking critical host: %s", critical_hosts[i]);
        if (!dns_resolve_check(critical_hosts[i])) {
            all_passed = false;
        }
    }

    if (all_passed) {
        console_info(&csl, "All DNS resolution checks passed");
    } else {
        console_error(&csl, "One or more DNS resolution checks failed");
    }

    return all_passed;
}

// Comprehensive API health check for all Fry APIs
bool comprehensive_api_health_check() {
    console_info(&csl, "Starting comprehensive API health checks");

    bool all_passed = true;

    // Check accounting API (existing implementation)
    if (!fry_check()) {
        all_passed = false;
    }

    // Check main API health endpoint
    char main_health_url[256];
    snprintf(main_health_url, sizeof(main_health_url), "%s", config.main_api);
    console_info(&csl, "Main API health url: %s", main_health_url);

    HttpGetOptions main_options = {
        .url = main_health_url,
        .bearer_token = NULL,
    };
    HttpResult main_result = http_get(&main_options);

    if (main_result.is_error) {
        console_error(&csl, "Main API health check failed: %s", main_result.error);
        all_passed = false;
    } else {
        console_info(&csl, "Main API is reachable");
    }

    if (main_result.response_buffer) {
        free(main_result.response_buffer);
    }

    // Check devices API health endpoint
    char devices_health_url[256];
    snprintf(devices_health_url, sizeof(devices_health_url), "%s/health", config.devices_api);
    console_info(&csl, "Devices API health url: %s", devices_health_url);

    HttpGetOptions devices_options = {
        .url = devices_health_url,
        .bearer_token = NULL,
    };
    HttpResult devices_result = http_get(&devices_options);

    if (devices_result.is_error) {
        console_error(&csl, "Devices API health check failed: %s", devices_result.error);
        all_passed = false;
    } else {
        console_info(&csl, "Devices API is reachable");
    }

    if (devices_result.response_buffer) {
        free(devices_result.response_buffer);
    }

    if (all_passed) {
        console_info(&csl, "All API health checks passed");
    } else {
        console_error(&csl, "One or more API health checks failed");
    }

    return all_passed;
}

// Initialize diagnostic service and run all init tests
bool init_diagnostic_service(DeviceInfo *device_info) {
    console_debug(&csl, "Initializing diagnostic service and running init tests");
    diagnostic_device_info = device_info;

    // 1. Comprehensive DNS resolution test (most fundamental)
    console_info(&csl, "=== Phase 1: DNS Resolution Tests ===");
    bool dns_status = comprehensive_dns_check();
    if (!dns_status) {
        console_error(&csl, "DNS resolution tests failed");
        return false;
    }

    // 2. Basic internet connectivity test
    console_info(&csl, "=== Phase 2: Internet Connectivity Test ===");
    bool internet_status = internet_check(config.external_connectivity_host);
    if (!internet_status) {
        console_error(&csl, "Internet connectivity test failed");
        return false;
    }

    // 3. Comprehensive API reachability tests
    console_info(&csl, "=== Phase 3: API Health Tests ===");
    bool api_status = comprehensive_api_health_check();
    if (!api_status) {
        console_error(&csl, "API health tests failed");
        return false;
    }

    console_info(&csl, "All diagnostic tests passed successfully");
    update_led_status(true, "All diagnostic tests passed");
    return true;
}

// Update LED status based on internet connectivity
void update_led_status(bool ok, const char *context) {
    if (strcmp(diagnostic_device_info->name, "Genesis") == 0 || strcmp(diagnostic_device_info->name, "Odyssey") == 0) {
        console_info(&csl, "Updating LEDs for device: %s", diagnostic_device_info->name, context);

        const char *blue_led =
            strcmp(diagnostic_device_info->name, "Odyssey") == 0 ? BLUE_LED_TRIGGER_ODYSSEY : BLUE_LED_TRIGGER;

        // console_info(&csl, "Device is Genesis. Updating LEDs. Context: %s", context);
        if (ok) {
            console_info(&csl, "Setting LED to indicate connectivity. Context: %s", context);
            set_led_trigger(GREEN_LED_TRIGGER, "default-on"); // Solid green
            set_led_trigger(RED_LED_TRIGGER, "none");
            set_led_trigger(blue_led, "none");
        } else {
            console_info(&csl, "Setting LED to indicate disconnection. Context: %s", context);
            set_led_trigger(GREEN_LED_TRIGGER, "none");
            set_led_trigger(RED_LED_TRIGGER, "timer"); // Blinking red
            set_led_trigger(blue_led, "none");
        }
    }
}

// Diagnostic task to check internet and update LED status
void diagnostic_task(void *task_context) {
    console_info(&csl, "Running periodic diagnostic task");

    // Check critical DNS resolution (subset for performance)
    // Only check the most critical domains that might be affected by network changes
    char *accounting_domain = extract_domain_from_url(config.accounting_api);
    if (accounting_domain) {
        if (!dns_resolve_check(accounting_domain)) {
            console_error(&csl, "Critical DNS resolution failed. Requesting exit.");
            update_led_status(false, "DNS check - Diagnostic task");
            free(accounting_domain);
            request_cleanup_and_exit("Critical DNS resolution failed during diagnostic task");
            return;
        }
        free(accounting_domain);
    }

    // Check internet status
    bool internet_status = internet_check(config.external_connectivity_host);
    console_info(&csl, "Diagnostic internet status: %s", internet_status ? "connected" : "disconnected");
    if (!internet_status) {
        console_error(&csl, "No internet connection. Requesting exit.");
        update_led_status(false, "Internet check - Diagnostic task");
        request_cleanup_and_exit("No internet connection during diagnostic task");
        return;
    }

    // Check accounting API reachability (most critical for core functionality)
    bool fry_status = fry_check();
    console_info(&csl, "Diagnostic fry status: %s", fry_status ? "reachable" : "unreachable");
    if (!fry_status) {
        console_error(&csl, "Fry is not reachable. Requesting exit.");
        update_led_status(false, "Fry check - Diagnostic task");
        request_cleanup_and_exit("Fry API unreachable during diagnostic task");
        return;
    }

    // Check valid token
    DiagnosticTaskContext *context = (DiagnosticTaskContext *)task_context;
    if (!is_token_valid(context->access_token)) {
        console_error(&csl, "Access token is invalid. Requesting exit.");
        update_led_status(false, "Access token check - Diagnostic task");
        request_cleanup_and_exit("Access token invalid during diagnostic task");
        return;
    }

    // All checks passed - update LED status to indicate healthy state
    update_led_status(true, "Diagnostic task - All checks passed");
    console_info(&csl, "All periodic diagnostic checks passed successfully");

    // No manual rescheduling needed - repeating tasks auto-reschedule
}

// Start diagnostic service
DiagnosticTaskContext *start_diagnostic_service(AccessToken *access_token) {
    DiagnosticTaskContext *context = (DiagnosticTaskContext *)malloc(sizeof(DiagnosticTaskContext));
    if (context == NULL) {
        console_error(&csl, "Failed to allocate memory for diagnostic task context");
        return NULL;
    }

    context->access_token = access_token;
    context->task_id = 0;

    // Convert seconds to milliseconds for scheduler
    uint32_t interval_ms = config.diagnostic_interval * 1000;
    uint32_t initial_delay_ms = config.diagnostic_interval * 1000; // Start after one interval

    console_info(&csl, "Starting diagnostic service with interval %u ms", interval_ms);

    // Schedule repeating task
    context->task_id = schedule_repeating(initial_delay_ms, interval_ms, diagnostic_task, context);

    if (context->task_id == 0) {
        console_error(&csl, "failed to schedule diagnostic task");
        free(context);
        return NULL;
    }

    console_debug(&csl, "Successfully scheduled diagnostic task with ID %u", context->task_id);
    return context;
}

void clean_diagnostic_context(DiagnosticTaskContext *context) {
    console_debug(&csl, "clean_diagnostic_context called with context: %p", context);
    if (context != NULL) {
        if (context->task_id != 0) {
            console_debug(&csl, "Cancelling diagnostic task %u", context->task_id);
            cancel_task(context->task_id);
        }
        console_debug(&csl, "Freeing diagnostic context %p", context);
        free(context);
    }
}
