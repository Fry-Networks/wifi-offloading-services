#ifndef DEVICE_STATUS_H
#define DEVICE_STATUS_H

#include "core/uloop_scheduler.h"
#include "services/access_token.h"
#include "services/device_info.h"
#include <stdbool.h>

typedef enum {
    Unknown = -1,
    Initial = 0,
    SetupPending = 1,
    SetupApproved = 2,
    MintPending = 3,
    Ready = 4,
    Banned = 5,
} DeviceStatus;

typedef struct {
    char *fry_device_id;
    DeviceInfo *device_info;
    AccessToken *access_token;
    task_id_t task_id; // Store current task ID for cleanup
} DeviceStatusTaskContext;

extern DeviceStatus device_status;

DeviceStatusTaskContext *
device_status_service(DeviceInfo *device_info, char *fry_device_id, AccessToken *access_token);
void clean_device_status_context(DeviceStatusTaskContext *context);

#endif /* DEVICE_STATUS_H */
