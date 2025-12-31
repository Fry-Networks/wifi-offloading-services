#include "core/console.h"
#include "core/uloop_scheduler.h"
#include "services/access_token.h"
#include "services/commands.h"
#include "services/config/config.h"
#include "services/device-context.h"
#include "services/device_info.h"
#include "services/device_status.h"
#include "services/diagnostic/diagnostic.h"
#include "services/exit_handler.h"
#include "services/firmware_upgrade.h"
#include "services/monitoring.h"
#include "services/mqtt/cert.h"
#include "services/mqtt/mqtt.h"
#include "services/nds.h"
#include "services/package_update.h"
#include "services/radsec_cert.h"
#include "services/reboot.h"
#include "services/registration.h"
#include "services/site-clients.h"
#include "services/speedtest.h"
#include "services/time_sync.h"

#include "services/ubus_server.h"
#include <libubox/uloop.h>

static Console csl = {
    .topic = "main",
};

int main(int argc, char *argv[]) {
    console_set_syslog_facility(CONSOLE_FACILITY_DAEMON);
    console_set_channels(CONSOLE_CHANNEL_SYSLOG | CONSOLE_CHANNEL_STDIO);
    console_set_identity("fry-agent");

    console_info(&csl, "starting fry-agent");

    // Initialize scheduler (includes uloop initialization)
    scheduler_init();
    console_info(&csl, "uloop scheduler initialized");

    // Verify scheduler is ready
    console_debug(&csl, "Scheduler initialization complete, proceeding with service setup");

    // Signal handlers
    setup_signal_handlers();

    // Config
    init_config(argc, argv);

    // DeviceInfo
    DeviceInfo *device_info = init_device_info();
    register_cleanup((cleanup_callback)clean_device_info, device_info);

    // Diagnostic Init - runs DNS, internet, and Fry reachability tests
    bool diagnostic_status = init_diagnostic_service(device_info);
    if (!diagnostic_status) {
        update_led_status(false, "Diagnostic tests failed");
        cleanup_and_exit(1, "Diagnostic tests failed");
    }

    // Registration
    Registration *registration =
        init_registration(device_info->mac, device_info->model, device_info->brand, device_info->device_id);
    register_cleanup((cleanup_callback)clean_registration, registration);

    // Access token
    AccessToken *access_token = init_access_token(registration);
    if (access_token == NULL) {
        console_error(&csl, "Failed to start access token ... exiting");
        cleanup_and_exit(1, "Failed to initialize access token");
    }
    register_cleanup((cleanup_callback)clean_access_token, access_token);

    // Package update complete signal
    check_package_update_completion(registration, device_info, access_token);

    // Firmware upgrade complete signal
    firmware_upgrade_on_boot(registration, device_info, access_token);

    // Certificate checks
    bool ca_cert_result = attempt_ca_cert(access_token);
    if (!ca_cert_result) cleanup_and_exit(1, "Failed to obtain CA certificate");

    bool generate_and_sign_result = attempt_generate_and_sign(access_token);
    if (!generate_and_sign_result) cleanup_and_exit(1, "Failed to generate and sign certificate");

    bool radsec_cert_result = attempt_radsec_ca_cert(access_token);
    if (!radsec_cert_result) cleanup_and_exit(1, "Failed to obtain RADSEC CA certificate");

    bool generate_and_sign_radsec_result = attempt_generate_and_sign_radsec(access_token, registration);
    if (!generate_and_sign_radsec_result) cleanup_and_exit(1, "Failed to generate and sign RADSEC certificate");

    install_radsec_cert();

    // Device context (site and config)
    DeviceContext *device_context = init_device_context(registration, access_token);
    register_cleanup((cleanup_callback)clean_device_context, device_context);

    // MQTT
    MqttConfig mqtt_config = {
        .client_id = registration->fry_device_id,
        .username = access_token->token,
        .password = "any",
        .broker_url = config.mqtt_broker_url,
        .data_path = config.data_path,
        .keepalive = config.mqtt_keepalive,
        .task_interval = config.mqtt_task_interval,
    };
    MqttClient mqtt_client = {
        .mosq = init_mqtt(&mqtt_config),
        .config = mqtt_config,
    };
    register_cleanup((cleanup_callback)cleanup_mqtt, &mqtt_client.mosq);

    // NDS
    NdsClient *nds_client = init_nds_client();
    register_cleanup((cleanup_callback)clean_nds_fifo, &nds_client->fifo_fd);

    // Site clients
    init_site_clients(mqtt_client.mosq, device_context->site, nds_client);

    // Register scheduler cleanup
    register_cleanup((cleanup_callback)scheduler_shutdown, NULL);

    // UBUS Server cleanup registration
    register_cleanup((cleanup_callback)ubus_server_cleanup, NULL);

    // Create MQTT access token refresh callback
    AccessTokenCallbacks token_callbacks = create_mqtt_token_callbacks(&mqtt_client);

    // Schedule service tasks - migrating to use the new uloop_scheduler API
    AccessTokenTaskContext *access_token_context = access_token_service(access_token, registration, &token_callbacks);
    if (access_token_context != NULL) {
        console_info(&csl, "Access token service started successfully");
        register_cleanup((cleanup_callback)clean_access_token_context, access_token_context);
    } else {
        console_error(&csl, "Failed to start access token service");
        cleanup_and_exit(1, "Failed to initialize access token service");
    }

    // Time sync service
    TimeSyncTaskContext *time_sync_context = time_sync_service();
    if (time_sync_context != NULL) {
        console_info(&csl, "Time sync service started successfully");
        register_cleanup((cleanup_callback)clean_time_sync_context, time_sync_context);
    } else {
        console_info(&csl, "Time sync service not started (dev mode or requirements not met)");
    }

    // MQTT service
    MqttTaskContext *mqtt_context = mqtt_service(mqtt_client.mosq, &mqtt_client.config);
    if (mqtt_context != NULL) {
        console_info(&csl, "MQTT service started successfully");
        register_cleanup((cleanup_callback)clean_mqtt_context, mqtt_context);
    } else {
        console_error(&csl, "Failed to start MQTT service");
        cleanup_and_exit(1, "Failed to initialize MQTT service");
    }

    // Device context service
    DeviceContextTaskContext *device_context_context =
        device_context_service(device_context, registration, access_token);
    if (device_context_context != NULL) {
        console_info(&csl, "Device context service started successfully");
        register_cleanup((cleanup_callback)clean_device_context_context, device_context_context);
    } else {
        console_error(&csl, "Failed to start device context service");
        cleanup_and_exit(1, "Failed to initialize device context service");
    }

    // Device status service
    DeviceStatusTaskContext *device_status_context =
        device_status_service(device_info, registration->fry_device_id, access_token);
    if (device_status_context != NULL) {
        console_info(&csl, "Device status service started successfully");
        register_cleanup((cleanup_callback)clean_device_status_context, device_status_context);
    } else {
        console_error(&csl, "Failed to start device status service");
        cleanup_and_exit(1, "Failed to initialize device status service");
    }

    // Reboot service
    RebootTaskContext *reboot_context = reboot_service();
    if (reboot_context != NULL) {
        console_info(&csl, "Reboot service started successfully");
        register_cleanup((cleanup_callback)clean_reboot_context, reboot_context);
    } else {
        console_info(&csl, "Reboot service not started (disabled in configuration)");
    }

    // NDS service
    NdsTaskContext *nds_context = nds_service(mqtt_client.mosq, device_context->site, nds_client, device_info);
    if (nds_context != NULL) {
        console_info(&csl, "NDS service started successfully");
        register_cleanup((cleanup_callback)clean_nds_context, nds_context);
    } else {
        console_info(&csl, "NDS service not started (dev mode or requirements not met)");
    }

    // Monitoring service
    MonitoringTaskContext *monitoring_context = monitoring_service(mqtt_client.mosq, registration);
    if (monitoring_context != NULL) {
        console_info(&csl, "Monitoring service started successfully");
        register_cleanup((cleanup_callback)clean_monitoring_context, monitoring_context);
    } else {
        console_info(&csl, "Monitoring service not started (disabled in configuration)");
    }

    // Firmware upgrade service
    FirmwareUpgradeTaskContext *firmware_context = firmware_upgrade_check(device_info, registration, access_token);
    if (firmware_context != NULL) {
        console_info(&csl, "Firmware upgrade service started successfully");
        register_cleanup((cleanup_callback)clean_firmware_upgrade_context, firmware_context);
    } else {
        console_error(&csl, "Failed to start firmware upgrade service");
        cleanup_and_exit(1, "Failed to initialize firmware upgrade service");
    }

    // Package update service
    PackageUpdateTaskContext *package_update_context = package_update_service(device_info, registration, access_token);
    if (package_update_context != NULL) {
        console_info(&csl, "Package update service started successfully");
        register_cleanup((cleanup_callback)clean_package_update_context, package_update_context);
    } else {
        console_error(&csl, "Failed to start package update service");
        cleanup_and_exit(1, "Failed to initialize package update service");
    }

    // Diagnostic service
    DiagnosticTaskContext *diagnostic_context = start_diagnostic_service(access_token);
    if (diagnostic_context != NULL) {
        console_info(&csl, "Diagnostic service started successfully");
        register_cleanup((cleanup_callback)clean_diagnostic_context, diagnostic_context);
    } else {
        console_error(&csl, "Failed to start diagnostic service");
        cleanup_and_exit(1, "Failed to initialize diagnostic service");
    }

    // Speedtest service
    SpeedTestTaskContext *speedtest_context = speedtest_service(mqtt_client.mosq, registration, access_token);
    if (speedtest_context != NULL) {
        console_info(&csl, "Speedtest service started successfully");
        register_cleanup((cleanup_callback)clean_speedtest_context, speedtest_context);
    } else {
        console_info(&csl, "Speedtest service not started (disabled in configuration)");
    }

    // Commands service (no scheduler needed - just sets up MQTT subscriptions)
    commands_service(mqtt_client.mosq, device_info, registration, access_token);
    console_info(&csl, "Commands service initialized successfully");

    // UBUS server service
    UbusServerTaskContext *ubus_context = ubus_server_service(access_token, device_info, registration);
    if (ubus_context != NULL) {
        console_info(&csl, "UBUS server service started successfully");
        register_cleanup((cleanup_callback)clean_ubus_server_context, ubus_context);
    } else {
        console_error(&csl, "Failed to start UBUS server service");
        cleanup_and_exit(1, "Failed to initialize UBUS server service");
    }

    console_debug(&csl, "All services initialized, about to start scheduler main loop");

    console_info(&csl, "Services scheduled, starting scheduler main loop");
    console_debug(&csl, "About to call scheduler_run()");
    int scheduler_result = scheduler_run();
    console_info(&csl, "Scheduler main loop ended with result: %d", scheduler_result);

    return 0;
}
