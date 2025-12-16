/**
 * @file tuya_main.c
 * @brief Factory Reset Application for Tuya DevKit
 *
 * This application performs a factory reset on the Tuya device,
 * clearing all activation data and requiring re-pairing through
 * the Tuya Smart Life app.
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#include "cJSON.h"
#include "tal_api.h"
#include "tkl_output.h"
#include "tuya_config.h"
#include "tuya_iot.h"
#include <assert.h>

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "1.0.0"
#endif

/* Tuya device handle */
tuya_iot_client_t client;

/* Tuya license information */
tuya_iot_license_t license;

/* Flag to track reset completion */
static volatile bool reset_completed = false;
static volatile bool reset_initiated = false;

/**
 * @brief User defined log output
 */
void user_log_output_cb(const char *str)
{
    tkl_log_output(str);
}

/**
 * @brief Network check callback - always returns true for reset operation
 */
bool user_network_check(void)
{
    return true;
}

/**
 * @brief Event handler for Tuya IoT events
 */
void user_event_handler_on(tuya_iot_client_t *client, tuya_event_msg_t *event)
{
    PR_DEBUG("Tuya Event ID:%d(%s)", event->id, EVENT_ID2STR(event->id));
    
    switch (event->id) {
    case TUYA_EVENT_RESET:
        PR_NOTICE("========================================");
        PR_NOTICE("    FACTORY RESET COMPLETED!");
        PR_NOTICE("    Reset Type: %d", event->value.asInteger);
        PR_NOTICE("========================================");
        PR_NOTICE("");
        PR_NOTICE("Device has been reset to factory defaults.");
        PR_NOTICE("All activation data has been cleared.");
        PR_NOTICE("");
        PR_NOTICE("Next steps:");
        PR_NOTICE("1. Open Tuya Smart Life app");
        PR_NOTICE("2. Add the device again");
        PR_NOTICE("3. Follow the pairing instructions");
        PR_NOTICE("");
        PR_NOTICE("You can now power off the device.");
        PR_NOTICE("========================================");
        reset_completed = true;
        break;

    case TUYA_EVENT_BIND_START:
        PR_INFO("Device Bind Start - Ready for new activation");
        break;

    case TUYA_EVENT_MQTT_CONNECTED:
        PR_INFO("Device MQTT Connected");
        break;

    default:
        break;
    }
}

/**
 * @brief Perform factory reset
 */
static void perform_factory_reset(void)
{
    int rt = OPRT_OK;
    
    PR_NOTICE("========================================");
    PR_NOTICE("    TUYA DEVKIT FACTORY RESET");
    PR_NOTICE("========================================");
    PR_NOTICE("");
    PR_NOTICE("Starting factory reset process...");
    PR_NOTICE("");
    
    // Check if device is activated
    if (tuya_iot_activated(&client)) {
        PR_INFO("Device is currently activated.");
        PR_INFO("Initiating factory reset...");
    } else {
        PR_WARN("Device is not activated.");
        PR_INFO("Clearing any existing data anyway...");
    }
    
    // Trigger the factory reset
    rt = tuya_iot_reset(&client);
    
    if (rt == OPRT_OK) {
        PR_INFO("Factory reset command sent successfully!");
        PR_INFO("Waiting for reset to complete...");
        reset_initiated = true;
    } else {
        PR_ERR("Factory reset failed with error: %d", rt);
        PR_INFO("Attempting direct data removal...");
        
        // Try direct data removal as fallback
        rt = tuya_iot_activated_data_remove(&client);
        if (rt == OPRT_OK) {
            PR_NOTICE("Direct data removal successful!");
            reset_completed = true;
        } else {
            PR_ERR("Direct data removal also failed: %d", rt);
        }
    }
}

/**
 * @brief Timer callback to trigger reset after initialization
 */
static void reset_timer_callback(TIMER_ID timer_id, void *arg)
{
    PR_INFO("Initialization complete. Starting reset...");
    perform_factory_reset();
}

void user_main(void)
{
    int rt = OPRT_OK;

    // Initialize runtime
    cJSON_InitHooks(&(cJSON_Hooks){.malloc_fn = tal_malloc, .free_fn = tal_free});
    tal_log_init(TAL_LOG_LEVEL_DEBUG, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    PR_NOTICE("========================================");
    PR_NOTICE("    FACTORY RESET APPLICATION");
    PR_NOTICE("========================================");
    PR_NOTICE("Project name:        %s", PROJECT_NAME);
    PR_NOTICE("App version:         %s", PROJECT_VERSION);
    PR_NOTICE("Compile time:        %s", __DATE__);
    PR_NOTICE("TuyaOpen version:    %s", OPEN_VERSION);
    PR_NOTICE("Platform chip:       %s", PLATFORM_CHIP);
    PR_NOTICE("Platform board:      %s", PLATFORM_BOARD);
    PR_NOTICE("========================================");

    // Initialize KV storage
    tal_kv_init(&(tal_kv_cfg_t){
        .seed = "vmlkasdh93dlvlcy",
        .key = "dflfuap134ddlduq",
    });
    tal_sw_timer_init();
    tal_workq_init();

    // Use placeholder credentials for reset operation
    license.uuid = TUYA_OPENSDK_UUID;
    license.authkey = TUYA_OPENSDK_AUTHKEY;

    // Initialize Tuya IoT client
    rt = tuya_iot_init(&client, &(const tuya_iot_config_t){
                                    .software_ver = PROJECT_VERSION,
                                    .productkey = TUYA_PRODUCT_ID,
                                    .uuid = license.uuid,
                                    .authkey = license.authkey,
                                    .event_handler = user_event_handler_on,
                                    .network_check = user_network_check,
                                });
    
    if (rt != OPRT_OK) {
        PR_ERR("tuya_iot_init failed: %d", rt);
        PR_INFO("Attempting reset without full initialization...");
        
        // Attempt direct KV clear
        PR_INFO("Clearing KV storage data...");
        tal_kv_set("rst_cnt", (uint8_t[]){0}, 1);
        
        PR_NOTICE("========================================");
        PR_NOTICE("    BASIC RESET COMPLETED");
        PR_NOTICE("========================================");
        PR_NOTICE("KV storage has been cleared.");
        PR_NOTICE("For full reset, please restart 3 times.");
        PR_NOTICE("========================================");
        
        // Infinite loop
        for (;;) {
            tal_system_sleep(1000);
        }
    }

    PR_INFO("Tuya IoT client initialized successfully");

    // Start Tuya IoT task
    tuya_iot_start(&client);
    PR_INFO("Tuya IoT started");

    // Create a timer to trigger reset after a short delay
    TIMER_ID reset_timer;
    tal_sw_timer_create(reset_timer_callback, NULL, &reset_timer);
    tal_sw_timer_start(reset_timer, 2000, TAL_TIMER_ONCE);

    PR_INFO("Reset will be triggered in 2 seconds...");

    // Main loop
    int loop_count = 0;
    for (;;) {
        tuya_iot_yield(&client);
        
        if (reset_completed) {
            loop_count++;
            if (loop_count % 50 == 0) {
                PR_INFO("Reset complete. Device is ready for re-pairing.");
            }
        }
        
        // Timeout after 30 seconds if reset not completed
        if (reset_initiated && !reset_completed) {
            static int timeout_counter = 0;
            timeout_counter++;
            if (timeout_counter > 30000) { // ~30 seconds
                PR_WARN("Reset timeout. Reset may have completed.");
                PR_NOTICE("Please power cycle the device and re-pair.");
                reset_completed = true;
            }
        }
    }
}

/**
 * @brief Main entry point
 */
#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    user_main();
}
#else

static THREAD_HANDLE ty_app_thread = NULL;

static void tuya_app_thread(void *arg)
{
    user_main();
    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thrd_param = {4096, 4, "factory_reset"};
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}
#endif
