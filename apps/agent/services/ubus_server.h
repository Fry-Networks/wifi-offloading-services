#ifndef UBUS_SERVER_H
#define UBUS_SERVER_H

#include "core/uloop_scheduler.h"
#include "services/access_token.h"
#include "services/device_info.h"
#include "services/registration.h"
#include <libubus.h>
#include <stdbool.h>

// UBUS service name for the agent
#define FRY_AGENT_SERVICE_NAME "fry-agent"

// UBUS method handler function type
typedef int (*UbusMethodHandler)(struct ubus_context *ctx,
                                 struct ubus_object *obj,
                                 struct ubus_request_data *req,
                                 const char *method,
                                 struct blob_attr *msg);

// Context structure for UBUS server - extensible for future services
typedef struct {
    AccessToken *access_token;
    DeviceInfo *device_info;
    Registration *registration;
    // Future services can be added here without breaking existing code
    void *reserved[8]; // Reserved pointers for future extensions
} UbusServerContext;

// Task context for scheduler integration
typedef struct {
    UbusServerContext *server_context;
    struct ubus_context *ubus_ctx;
    task_id_t task_id; // Store current task ID for cleanup
} UbusServerTaskContext;

/**
 * Initialize the UBUS server
 * @param access_token Pointer to access token for token-related methods
 * @param device_info Pointer to device info for device-related methods
 * @param registration Pointer to registration for device ID methods
 * @return 0 on success, negative error code on failure
 */
int ubus_server_init(AccessToken *access_token, DeviceInfo *device_info, Registration *registration);

/**
 * Start the UBUS server service (integrates with scheduler)
 * @param access_token Pointer to access token
 * @param device_info Pointer to device info
 * @param registration Pointer to registration
 * @return Pointer to task context for cleanup, NULL on failure
 */
UbusServerTaskContext *
ubus_server_service(AccessToken *access_token, DeviceInfo *device_info, Registration *registration);

/**
 * UBUS server task function for scheduler
 * @param context Task context containing server data
 */
void ubus_server_task(void *context);

/**
 * Cleanup UBUS server task context
 * @param context Task context to cleanup
 */
void clean_ubus_server_context(UbusServerTaskContext *context);

/**
 * Cleanup the UBUS server
 */
void ubus_server_cleanup(void);

/**
 * Check if UBUS server is running
 * @return true if running, false otherwise
 */
bool ubus_server_is_running(void);

/**
 * Get the UBUS context (for internal use)
 * @return Pointer to UBUS context or NULL if not initialized
 */
struct ubus_context *ubus_server_get_context(void);

#endif // UBUS_SERVER_H