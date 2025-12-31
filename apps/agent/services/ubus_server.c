#include "ubus_server.h"
#include "core/console.h"
#include <asm-generic/errno.h>
#include <json-c/json.h>
#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubus.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Polling interval for UBUS events (milliseconds)
#define UBUS_POLL_INTERVAL_MS 100
#define UBUS_TASK_INTERVAL_SECONDS 1

static Console csl = {
    .topic = "ubus_server",
};

// Global server state
static struct ubus_context *ubus_ctx = NULL;
static struct ubus_object fry_object;
static UbusServerContext *server_context = NULL;
static bool server_running = false;

// Method handlers
static int method_get_access_token(struct ubus_context *ctx,
                                   struct ubus_object *obj,
                                   struct ubus_request_data *req,
                                   const char *method,
                                   struct blob_attr *msg);

static int method_get_device_info(struct ubus_context *ctx,
                                  struct ubus_object *obj,
                                  struct ubus_request_data *req,
                                  const char *method,
                                  struct blob_attr *msg);

static int method_get_status(struct ubus_context *ctx,
                             struct ubus_object *obj,
                             struct ubus_request_data *req,
                             const char *method,
                             struct blob_attr *msg);

static int method_get_registration(struct ubus_context *ctx,
                                   struct ubus_object *obj,
                                   struct ubus_request_data *req,
                                   const char *method,
                                   struct blob_attr *msg);

static int method_ping(struct ubus_context *ctx,
                       struct ubus_object *obj,
                       struct ubus_request_data *req,
                       const char *method,
                       struct blob_attr *msg);

// UBUS method definitions - extensible for future methods
static const struct ubus_method fry_methods[] = {
    UBUS_METHOD_NOARG("get_access_token", method_get_access_token),
    UBUS_METHOD_NOARG("get_device_info", method_get_device_info),
    UBUS_METHOD_NOARG("get_status", method_get_status),
    UBUS_METHOD_NOARG("get_registration", method_get_registration),
    UBUS_METHOD_NOARG("ping", method_ping),
};

static struct ubus_object_type fry_object_type = UBUS_OBJECT_TYPE(FRY_AGENT_SERVICE_NAME, fry_methods);

static struct ubus_object fry_object = {
    .name = FRY_AGENT_SERVICE_NAME,
    .type = &fry_object_type,
    .methods = fry_methods,
    .n_methods = ARRAY_SIZE(fry_methods),
};

// Helper function to send JSON error response
static void
send_error_response(struct ubus_context *ctx, struct ubus_request_data *req, const char *error_msg, int error_code) {
    struct blob_buf response = {0};
    blob_buf_init(&response, 0);
    blobmsg_add_string(&response, "error", error_msg);
    blobmsg_add_u32(&response, "code", error_code);
    ubus_send_reply(ctx, req, response.head);
    blob_buf_free(&response);
}

// Method: get_access_token
static int method_get_access_token(struct ubus_context *ctx,
                                   struct ubus_object *obj,
                                   struct ubus_request_data *req,
                                   const char *method,
                                   struct blob_attr *msg) {
    console_debug(&csl, "UBUS method called: %s", method);

    if (!server_context || !server_context->access_token) {
        send_error_response(ctx, req, "Access token not available", -ENODATA);
        return -ENODATA;
    }

    AccessToken *token = server_context->access_token;
    struct blob_buf response = {0};
    blob_buf_init(&response, 0);

    if (token->token) {
        blobmsg_add_string(&response, "token", token->token);
        blobmsg_add_u64(&response, "issued_at", token->issued_at_seconds);
        blobmsg_add_u64(&response, "expires_at", token->expires_at_seconds);
        blobmsg_add_u8(&response, "valid", is_token_valid(token) ? 1 : 0);
    } else {
        blobmsg_add_string(&response, "error", "Token not initialized");
    }

    int ret = ubus_send_reply(ctx, req, response.head);
    blob_buf_free(&response);
    return ret;
}

// Method: get_device_info
static int method_get_device_info(struct ubus_context *ctx,
                                  struct ubus_object *obj,
                                  struct ubus_request_data *req,
                                  const char *method,
                                  struct blob_attr *msg) {
    console_debug(&csl, "UBUS method called: %s", method);

    if (!server_context || !server_context->device_info) {
        send_error_response(ctx, req, "Device info not available", -ENODATA);
        return -ENODATA;
    }

    DeviceInfo *info = server_context->device_info;
    struct blob_buf response = {0};
    blob_buf_init(&response, 0);

    if (info->device_id) blobmsg_add_string(&response, "device_id", info->device_id);
    if (info->mac) blobmsg_add_string(&response, "mac", info->mac);
    if (info->name) blobmsg_add_string(&response, "name", info->name);
    if (info->brand) blobmsg_add_string(&response, "brand", info->brand);
    if (info->model) blobmsg_add_string(&response, "model", info->model);
    if (info->arch) blobmsg_add_string(&response, "arch", info->arch);
    if (info->public_ip) blobmsg_add_string(&response, "public_ip", info->public_ip);
    if (info->os_name) blobmsg_add_string(&response, "os_name", info->os_name);
    if (info->os_version) blobmsg_add_string(&response, "os_version", info->os_version);
    if (info->os_services_version) blobmsg_add_string(&response, "os_services_version", info->os_services_version);
    if (info->did_public_key) blobmsg_add_string(&response, "did_public_key", info->did_public_key);

    int ret = ubus_send_reply(ctx, req, response.head);
    blob_buf_free(&response);
    return ret;
}

// Method: get_status
static int method_get_status(struct ubus_context *ctx,
                             struct ubus_object *obj,
                             struct ubus_request_data *req,
                             const char *method,
                             struct blob_attr *msg) {
    console_debug(&csl, "UBUS method called: %s", method);

    struct blob_buf response = {0};
    blob_buf_init(&response, 0);

    blobmsg_add_string(&response, "service", FRY_AGENT_SERVICE_NAME);
    blobmsg_add_u8(&response, "running", server_running ? 1 : 0);

    if (server_context) {
        blobmsg_add_u8(&response, "access_token_available", server_context->access_token != NULL ? 1 : 0);
        blobmsg_add_u8(&response, "device_info_available", server_context->device_info != NULL ? 1 : 0);
        blobmsg_add_u8(&response, "registration_available", server_context->registration != NULL ? 1 : 0);

        if (server_context->access_token) {
            blobmsg_add_u8(&response, "token_valid", is_token_valid(server_context->access_token) ? 1 : 0);
        }
    }

    int ret = ubus_send_reply(ctx, req, response.head);
    blob_buf_free(&response);
    return ret;
}

// Method: get_registration
static int method_get_registration(struct ubus_context *ctx,
                                   struct ubus_object *obj,
                                   struct ubus_request_data *req,
                                   const char *method,
                                   struct blob_attr *msg) {
    console_debug(&csl, "UBUS method called: %s", method);

    if (!server_context || !server_context->registration) {
        send_error_response(ctx, req, "Registration not available", -ENODATA);
        return -ENODATA;
    }

    Registration *reg = server_context->registration;
    struct blob_buf response = {0};
    blob_buf_init(&response, 0);

    if (reg->fry_device_id) blobmsg_add_string(&response, "fry_device_id", reg->fry_device_id);
    if (reg->access_key) blobmsg_add_string(&response, "access_key", reg->access_key);

    int ret = ubus_send_reply(ctx, req, response.head);
    blob_buf_free(&response);
    return ret;
}

// Method: ping
static int method_ping(struct ubus_context *ctx,
                       struct ubus_object *obj,
                       struct ubus_request_data *req,
                       const char *method,
                       struct blob_attr *msg) {
    console_debug(&csl, "UBUS method called: %s", method);

    struct blob_buf response = {0};
    blob_buf_init(&response, 0);

    blobmsg_add_string(&response, "response", "pong");
    blobmsg_add_string(&response, "service", FRY_AGENT_SERVICE_NAME);
    blobmsg_add_u64(&response, "timestamp", time(NULL));

    int ret = ubus_send_reply(ctx, req, response.head);
    blob_buf_free(&response);
    return ret;
}

// UBUS server task for scheduler integration
void ubus_server_task(void *context) {
    UbusServerTaskContext *task_ctx = (UbusServerTaskContext *)context;

    if (!task_ctx || !task_ctx->ubus_ctx) {
        console_error(&csl, "Invalid task context");
        return;
    }

    // Check if UBUS connection is still alive
    // uloop handles the actual event processing now
    if (!ubus_server_is_running()) {
        console_warn(&csl, "UBUS connection lost, attempting reconnect");
        ubus_server_cleanup();
        // Reinitialize with current context
        if (server_context) {
            int ret = ubus_server_init(server_context->access_token, server_context->device_info,
                                       server_context->registration);
            if (ret < 0) {
                console_error(&csl, "Failed to reconnect UBUS server: %d", ret);
            }
        }
    }

    // No manual rescheduling needed - repeating tasks auto-reschedule
}

// Initialize UBUS server
int ubus_server_init(AccessToken *access_token, DeviceInfo *device_info, Registration *registration) {
    console_info(&csl, "Initializing UBUS server as '%s'", FRY_AGENT_SERVICE_NAME);

    if (server_running) {
        console_warn(&csl, "UBUS server already running");
        return 0;
    }

    // Allocate server context
    server_context = malloc(sizeof(UbusServerContext));
    if (!server_context) {
        console_error(&csl, "Failed to allocate server context");
        return -ENOMEM;
    }

    // Initialize context
    memset(server_context, 0, sizeof(UbusServerContext));
    server_context->access_token = access_token;
    server_context->device_info = device_info;
    server_context->registration = registration;

    // Connect to UBUS (uloop already initialized in main)
    ubus_ctx = ubus_connect(NULL);
    if (!ubus_ctx) {
        console_error(&csl, "Failed to connect to UBUS");
        free(server_context);
        server_context = NULL;
        return -ECONNREFUSED;
    }

    // Add UBUS to uloop
    ubus_add_uloop(ubus_ctx);

    // Register our object
    int ret = ubus_add_object(ubus_ctx, &fry_object);
    if (ret) {
        console_error(&csl, "Failed to add UBUS object: %s", ubus_strerror(ret));
        ubus_free(ubus_ctx);
        ubus_ctx = NULL;
        free(server_context);
        server_context = NULL;
        return -EBADFD;
    }

    server_running = true;
    console_info(&csl, "UBUS server initialized successfully");

    return 0;
}

// Start UBUS server service with scheduler integration
UbusServerTaskContext *
ubus_server_service(AccessToken *access_token, DeviceInfo *device_info, Registration *registration) {
    console_info(&csl, "Starting UBUS server service");

    // Initialize server if not already done
    if (!server_running) {
        int ret = ubus_server_init(access_token, device_info, registration);
        if (ret < 0) {
            console_error(&csl, "Failed to initialize UBUS server: %d", ret);
            return NULL;
        }
    }

    // Create task context
    UbusServerTaskContext *task_ctx = malloc(sizeof(UbusServerTaskContext));
    if (!task_ctx) {
        console_error(&csl, "Failed to allocate task context");
        return NULL;
    }

    task_ctx->server_context = server_context;
    task_ctx->ubus_ctx = ubus_ctx;
    task_ctx->task_id = 0;

    // Convert seconds to milliseconds for scheduler
    uint32_t interval_ms = UBUS_TASK_INTERVAL_SECONDS * 1000;
    uint32_t initial_delay_ms = UBUS_TASK_INTERVAL_SECONDS * 1000; // Start after one interval

    console_info(&csl, "Starting UBUS server service with interval %u ms", interval_ms);

    // Schedule repeating task
    task_ctx->task_id = schedule_repeating(initial_delay_ms, interval_ms, ubus_server_task, task_ctx);

    if (task_ctx->task_id == 0) {
        console_error(&csl, "failed to schedule UBUS server task");
        free(task_ctx);
        return NULL;
    }

    console_debug(&csl, "Successfully scheduled UBUS server task with ID %u", task_ctx->task_id);
    console_info(&csl, "UBUS server service started");
    return task_ctx;
}

void clean_ubus_server_context(UbusServerTaskContext *context) {
    console_debug(&csl, "clean_ubus_server_context called with context: %p", context);
    if (context != NULL) {
        if (context->task_id != 0) {
            console_debug(&csl, "Cancelling UBUS server task %u", context->task_id);
            cancel_task(context->task_id);
        }
        console_debug(&csl, "Freeing UBUS server context %p", context);
        free(context);
    }
}

// Cleanup UBUS server
void ubus_server_cleanup(void) {
    console_info(&csl, "Cleaning up UBUS server");

    if (ubus_ctx) {
        ubus_remove_object(ubus_ctx, &fry_object);
        ubus_free(ubus_ctx);
        ubus_ctx = NULL;
    }

    if (server_context) {
        free(server_context);
        server_context = NULL;
    }

    server_running = false;
    console_info(&csl, "UBUS server cleanup complete");
}

// Check if server is running
bool ubus_server_is_running(void) { return server_running && ubus_ctx != NULL; }

// Get UBUS context (for internal use)
struct ubus_context *ubus_server_get_context(void) { return ubus_ctx; }
