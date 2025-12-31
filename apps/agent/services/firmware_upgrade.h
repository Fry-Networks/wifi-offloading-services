#ifndef FIRMWARE_UPGRADE_H
#define FIRMWARE_UPGRADE_H

#include "core/uloop_scheduler.h"
#include "services/access_token.h"
#include "services/device_info.h"
#include "services/registration.h"

void firmware_upgrade_on_boot(Registration *registration, DeviceInfo *device_info, AccessToken *access_token);
typedef struct {
    DeviceInfo *device_info;
    Registration *registration;
    AccessToken *access_token;
    task_id_t task_id; // Store current task ID for cleanup
} FirmwareUpgradeTaskContext;

FirmwareUpgradeTaskContext *
firmware_upgrade_check(DeviceInfo *device_info, Registration *registration, AccessToken *access_token);
void clean_firmware_upgrade_context(FirmwareUpgradeTaskContext *context);
void send_firmware_check_request(const char *codename,
                                 const char *version,
                                 const char *fry_device_id,
                                 AccessToken *access_token);
void clean_firmware_upgrade_service();

#endif // FIRMWARE_UPGRADE_H
