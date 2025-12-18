/**
 * @file cli_cmd.c
 * @brief Command Line Interface (CLI) commands for Tuya IoT applications.
 *
 * This file implements a set of CLI commands for controlling and managing Tuya
 * IoT devices. It includes commands for switching device states, executing
 * system commands, managing key-value pairs, resetting and starting/stopping
 * the IoT process, and retrieving memory usage information. These commands
 * facilitate debugging, testing, and managing Tuya IoT applications directly
 * from a command line interface.
 *
 * Key functionalities provided in this file:
 * - Switching device states (on/off).
 * - Executing arbitrary system commands.
 * - Key-value pair management for device configuration.
 * - Resetting, starting, and stopping the IoT process.
 * - Retrieving current free heap memory size.
 *
 * This implementation leverages Tuya's Application Layer (TAL) APIs and IoT SDK
 * to provide a rich set of commands for device management and debugging. It is
 * designed to enhance the development and testing process of Tuya IoT
 * applications.
 *
 * @copyright Copyright (c) 2021-2024 Tuya Inc. All Rights Reserved.
 *
 */

#include "tal_api.h"
#include "tuya_iot.h"
#include <stdlib.h>

/* Audio includes for testing */
#include "ai_audio_player.h"
#include "ai_audio.h"
#include "alert_audio_data.h"

/* BLE configuration for Web Bluetooth */
#include "ble_config.h"

extern void tal_kv_cmd(int argc, char *argv[]);
extern void netmgr_cmd(int argc, char *argv[]);

/**
 * @brief switch demo on/off cmd
 *
 * @param argc
 * @param argv
 * @return void
 */
static void switch_test(int argc, char *argv[])
{
    if (argc < 2) {
        PR_INFO("usge: switch <on/off>");
        return;
    }

    char bool_value[128];
    if (0 == strcmp(argv[1], "on")) {
        sprintf(bool_value, "{\"1\": true}");
    } else if (0 == strcmp(argv[1], "off")) {
        sprintf(bool_value, "{\"1\": false}");
    } else {
        PR_INFO("usge: switch <on/off>");
        return;
    }

    tuya_iot_dp_report_json(tuya_iot_client_get(), bool_value);
}

/**
 * @brief excute system cmd
 *
 * @param argc
 * @param argv
 * @return void
 */
static void system_cmd(int argc, char *argv[])
{
    char cmd[256];

    if (argc < 2) {
        PR_INFO("usge: sys <cmd>");
        return;
    }

    size_t offset = 0;

    for (int i = 1; i < argc; i++) {
        int ret = snprintf(cmd + offset, sizeof(cmd) - offset, "%s ", argv[i]);
        if (ret < 0 || offset + ret >= sizeof(cmd)) {
            break;
        }
        offset += ret;
    }

    PR_DEBUG("system %s", cmd);
    system(cmd);
}

/**
 * @brief get free heap size cmd
 *
 * @param argc
 * @param argv
 */
static void mem(int argc, char *argv[])
{
    int free_heap = 0;
    free_heap = tal_system_get_free_heap_size();
    PR_NOTICE("cur free heap: %d", free_heap);
}

/**
 * @brief reset iot to unactive/unregister
 *
 * @param argc
 * @param argv
 */
static void reset(int argc, char *argv[])
{
    tuya_iot_reset(tuya_iot_client_get());
}

/**
 * @brief reset iot to unactive/unregister
 *
 * @param argc
 * @param argv
 */
static void start(int argc, char *argv[])
{
    tuya_iot_start(tuya_iot_client_get());
}

/**
 * @brief stop iot
 *
 * @param argc
 * @param argv
 */
static void stop(int argc, char *argv[])
{
    tuya_iot_stop(tuya_iot_client_get());
}

/**
 * @brief audio test command - play alert sound or set volume
 *
 * @param argc
 * @param argv
 */
static void audio_test(int argc, char *argv[])
{
    OPERATE_RET rt = OPRT_OK;
    
    if (argc < 2) {
        PR_INFO("usage: audio <play|stop|vol <0-100>>");
        PR_INFO("  play  - play alert sound");
        PR_INFO("  stop  - stop playing");
        PR_INFO("  vol   - set volume (0-100)");
        return;
    }
    
    if (0 == strcmp(argv[1], "play")) {
        PR_INFO("Starting audio playback...");
        
        /* Stop any currently playing audio */
        if (ai_audio_player_is_playing()) {
            PR_INFO("Stopping previous audio...");
            ai_audio_player_stop();
        }
        
        /* Start the player */
        rt = ai_audio_player_start("cli_test");
        if (rt != OPRT_OK) {
            PR_ERR("Failed to start audio player: %d", rt);
            return;
        }
        PR_INFO("Audio player started");
        
        /* Write audio data */
        rt = ai_audio_player_data_write("cli_test", 
                                         (uint8_t *)alert_audio_data, 
                                         sizeof(alert_audio_data), 
                                         1);
        if (rt != OPRT_OK) {
            PR_ERR("Failed to write audio data: %d", rt);
        } else {
            PR_INFO("Audio data written (%d bytes), playing...", sizeof(alert_audio_data));
        }
    } else if (0 == strcmp(argv[1], "stop")) {
        PR_INFO("Stopping audio...");
        ai_audio_player_stop();
        PR_INFO("Audio stopped");
    } else if (0 == strcmp(argv[1], "vol")) {
        if (argc < 3) {
            PR_INFO("Current volume: %d", ai_audio_get_volume());
            return;
        }
        uint8_t vol = (uint8_t)atoi(argv[2]);
        if (vol > 100) vol = 100;
        PR_INFO("Setting volume to %d...", vol);
        rt = ai_audio_set_volume(vol);
        if (rt != OPRT_OK) {
            PR_ERR("Failed to set volume: %d", rt);
        } else {
            PR_INFO("Volume set to %d", vol);
        }
    } else {
        PR_INFO("Unknown command: %s", argv[1]);
        PR_INFO("usage: audio <play|stop|vol <0-100>>");
    }
}

/**
 * @brief Show current TCP server settings
 *
 * @param argc
 * @param argv
 */
static void config_show(int argc, char *argv[])
{
    char host[64] = "";
    uint16_t port = 5000;
    char token[64] = "";
    
    if (ble_config_load_tcp_settings(host, &port, token) == OPRT_OK) {
        PR_NOTICE("Saved TCP Settings:");
        PR_NOTICE("  Host:  %s", host);
        PR_NOTICE("  Port:  %d", port);
        PR_NOTICE("  Token: %s", token[0] ? "****" : "(not set)");
    } else {
        PR_NOTICE("No saved TCP settings found.");
        PR_NOTICE("Using compile-time defaults from .env file.");
    }
    
    PR_NOTICE("");
    PR_NOTICE("To configure via Bluetooth:");
    PR_NOTICE("  1. Open https://ble-config-web.vercel.app");
    PR_NOTICE("  2. Connect to this device");
    PR_NOTICE("  3. Update settings and save");
}

/**
 * @brief cli cmd list
 *
 */
static cli_cmd_t s_cli_cmd[] = {
    {.name = "switch", .func = switch_test, .help = "switch test"},
    {.name = "kv", .func = tal_kv_cmd, .help = "kv test"},
    {.name = "sys", .func = system_cmd, .help = "system  cmd"},
    {.name = "reset", .func = reset, .help = "reset iot"},
    {.name = "stop", .func = stop, .help = "stop iot"},
    {.name = "start", .func = start, .help = "start iot"},
    {.name = "mem", .func = mem, .help = "mem size"},
    {.name = "netmgr", .func = netmgr_cmd, .help = "netmgr cmd"},
    {.name = "audio", .func = audio_test, .help = "audio test <play|stop|vol>"},
    {.name = "config", .func = config_show, .help = "show TCP config & BLE setup info"},
};

/**
 * @brief
 *
 */
void tuya_app_cli_init(void)
{
    tal_cli_cmd_register(s_cli_cmd, sizeof(s_cli_cmd) / sizeof(s_cli_cmd[0]));
}