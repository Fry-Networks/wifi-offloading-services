#include "openwisp_manager.h"
#include "core/console.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static Console csl = {.topic = "openwisp-mgr"};

// Script paths
#define DEV_OPENWISP_SCRIPT "./scripts/openwisp_config.uc"
#define PROD_OPENWISP_SCRIPT "/etc/fry-config/scripts/openwisp_config.uc"

#define DEV_UCODE_PATH "/usr/local/bin/ucode"
#define PROD_UCODE_PATH "/usr/bin/ucode"

// Parse and forward script logs to fry-config console
static void parse_and_log_script_output(const char *line) {
    if (!line || strlen(line) == 0) return;

    // Parse script log format: [openwisp-config] LEVEL: message
    if (strstr(line, "[openwisp-config] INFO:")) {
        const char *msg = strstr(line, "INFO:") + 5;
        while (*msg == ' ') msg++; // Skip spaces
        console_info(&csl, "Openwisp script: %s", msg);

    } else if (strstr(line, "[openwisp-config] WARN:")) {
        const char *msg = strstr(line, "WARN:") + 5;
        while (*msg == ' ') msg++; // Skip spaces
        console_warn(&csl, "Openwisp script: %s", msg);
        
    } else if (strstr(line, "[openwisp-config] DEBUG:")) {
        const char *msg = strstr(line, "DEBUG:") + 6;
        while (*msg == ' ') msg++; // Skip spaces
        console_debug(&csl, "Openwisp script: %s", msg);
        
    } else if (strstr(line, "[openwisp-config]")) {
        // Generic openwisp-config log without specific level
        const char *msg = strstr(line, "] ") + 2;
        console_info(&csl, "Openwisp script: %s", msg);
        
    } else if (strstr(line, "config controller") || 
               strstr(line, "list unmanaged") || 
               strstr(line, "option url")) {
        // UCI configuration output
        console_debug(&csl, "Openwisp config: %s", line);
        
    } else if (strlen(line) > 0 && line[0] != '\n') {
        // Any other non-empty output
        console_debug(&csl, "Openwisp output: %s", line);
    }
}

// Execute the OpenWisp script with integrated logging
static int execute_openwisp_script(bool dev_mode) {
    const char *script_path = dev_mode ? DEV_OPENWISP_SCRIPT : PROD_OPENWISP_SCRIPT;
    const char *ucode_path = dev_mode ? DEV_UCODE_PATH : PROD_UCODE_PATH;

    // Verify ucode exists
    if (access(ucode_path, X_OK) != 0) {
        console_error(&csl, "ucode not found at %s", ucode_path);
        return -1;
    }

    // Verify script exists
    if (access(script_path, R_OK) != 0) {
        console_error(&csl, "Openwisp script not found at %s", script_path);
        console_error(&csl, "Please ensure the script is installed properly");
        return -1;
    }

    // Execute script
    char command[512];
    snprintf(command, sizeof(command), "%s %s 2>&1", ucode_path, script_path);

    console_debug(&csl, "Executing Openwisp script: %s", script_path);

    FILE *fp = popen(command, "r");
    if (!fp) {
        console_error(&csl, "Failed to execute Openwisp script");
        return -1;
    }

    // Read and parse script output line by line
    char buffer[512];
    int line_count = 0;
    
    while (fgets(buffer, sizeof(buffer), fp)) {
        // Remove newline
        char *newline = strchr(buffer, '\n');
        if (newline) *newline = '\0';
        
        if (strlen(buffer) > 0) {
            parse_and_log_script_output(buffer);
            line_count++;
        }
    }

    int exit_code = pclose(fp);
    int script_exit_code = WEXITSTATUS(exit_code);

    if (line_count == 0) {
        console_debug(&csl, "Openwisp script produced no output");
    } else {
        console_debug(&csl, "Openwisp script completed with %d log lines", line_count);
    }

    return script_exit_code;
}

// Configure Openwisp to ignore fry-managed sections
int configure_openwisp_exclusions(bool dev_mode) {
    console_info(&csl, "Configuring Openwisp exclusions for fry-managed sections...");

    int result = execute_openwisp_script(dev_mode);
    
    if (result < 0) {
        console_error(&csl, "Openwisp configuration script failed");
        return -1;
    } else if (result > 0) {
        console_info(&csl, "Openwisp configuration updated (%d changes)", result);
    } else {
        console_info(&csl, "Openwisp configuration already correct");
    }

    return 0;
}