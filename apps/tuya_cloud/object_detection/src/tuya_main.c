/**
 * @file tuya_main.c
 * @brief Object Detection module with audio alert functionality.
 *
 * This application plays audio alerts when the detection switch is toggled.
 * When ON: Plays an alert sound
 * When OFF: Stops any playing audio
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#include "cJSON.h"
#include "netmgr.h"
#include "tal_api.h"
#include "tkl_output.h"
#include "tuya_config.h"
#include "tuya_iot.h"
#include "tuya_iot_dp.h"
#include "tal_cli.h"
#include "tuya_authorize.h"
#include <assert.h>
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
#include "netconn_wifi.h"
#endif
#if defined(ENABLE_WIRED) && (ENABLE_WIRED == 1)
#include "netconn_wired.h"
#endif
#if defined(ENABLE_CELLULAR) && (ENABLE_CELLULAR == 1)
#include "netconn_cellular.h"
#endif
#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
#include "lwip_init.h"
#endif

#include "reset_netcfg.h"
#include "tkl_gpio.h"

#if defined(ENABLE_QRCODE) && (ENABLE_QRCODE == 1)
#include "qrencode_print.h"
#endif

/* Audio player includes */
#include "ai_audio_player.h"
#include "ai_audio.h"
#include "alert_audio_data.h"
#include "tdl_audio_manage.h"

/* TCP client for web app communication */
#include "tcp_client.h"

/* BLE configuration for Web Bluetooth */
#include "ble_config.h"

/* Microphone streaming for web app */
#include "mic_streaming.h"

/* Switch DP ID - typically DP 1 for switch products */
#define SWITCH_DP_ID         1
/* Volume DP ID - DP 3 for volume control */
#define VOLUME_DP_ID         3
/* Default volume level (0-100) */
#define DEFAULT_VOLUME       70

/* TCP Server defaults (can be overridden via .env) */
#ifndef TCP_SERVER_HOST
#define TCP_SERVER_HOST "192.168.18.10"
#endif
#ifndef TCP_SERVER_PORT
#define TCP_SERVER_PORT 5000
#endif

/* Current detection state */
static bool g_detection_active = false;

/* Audio player initialized flag */
static bool g_audio_initialized = false;

/* Current volume level (0-100) */
static uint8_t g_current_volume = DEFAULT_VOLUME;

/* Speaker enable GPIO (T5AI-CORE uses GPIO39) */
#define SPEAKER_EN_GPIO TUYA_GPIO_NUM_39

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "1.0.0"
#endif

/* for cli command register */
extern void tuya_app_cli_init(void);

/* Board hardware registration (audio driver, etc.) */
extern OPERATE_RET board_register_hardware(void);

/* Tuya device handle */
tuya_iot_client_t client;

/* Tuya license information (uuid authkey) */
tuya_iot_license_t license;

/**
 * @brief Callback for messages received from web app via TCP
 */
static void tcp_message_callback(const char *data, uint32_t len)
{
    PR_INFO("Web App Command: %.*s", len, data);
    
    char response[256];
    
    /* Handle server responses (not commands) */
    if (strncmp(data, "auth:ok", 7) == 0) {
        PR_INFO("Server authenticated us successfully");
        return;  /* Don't process as a command */
    }
    
    /* Handle different commands from web app */
    if (strncmp(data, "ping", 4) == 0) {
        tcp_client_send_str("pong");
    }
    else if (strncmp(data, "test", 4) == 0) {
        /* Simple test command to verify callback works */
        tcp_client_send_str("ok:test_received");
    }
    else if (strncmp(data, "status", 6) == 0) {
        /* Simplified status response */
        int heap = tal_system_get_free_heap_size();
        snprintf(response, sizeof(response), 
            "{\"detection\":%s,\"volume\":%d,\"audio_init\":%s,\"mic_streaming\":%s,\"heap\":%d}",
            g_detection_active ? "true" : "false",
            g_current_volume,
            g_audio_initialized ? "true" : "false",
            mic_streaming_is_active() ? "true" : "false",
            heap);
        tcp_client_send_str(response);
    }
    else if (strncmp(data, "audio play", 10) == 0) {
        if (g_audio_initialized && g_current_volume > 0) {
            PR_INFO("Web App triggered audio play");
            if (ai_audio_player_is_playing()) {
                ai_audio_player_stop();
            }
            ai_audio_player_start("webapp_audio");
            ai_audio_player_data_write("webapp_audio", 
                                       (uint8_t *)alert_audio_data, 
                                       sizeof(alert_audio_data), 1);
            tcp_client_send_str("ok:audio_playing");
        } else {
            tcp_client_send_str("error:audio_not_ready");
        }
    }
    else if (strncmp(data, "audio stop", 10) == 0) {
        if (ai_audio_player_is_playing()) {
            ai_audio_player_stop();
        }
        tcp_client_send_str("ok:audio_stopped");
    }
    else if (strncmp(data, "mic on", 6) == 0) {
        /* Start microphone streaming to web app */
        if (mic_streaming_is_active()) {
            tcp_client_send_str("ok:mic_already_on");
        } else {
            OPERATE_RET rt = mic_streaming_start();
            if (rt == OPRT_OK) {
                tcp_client_send_str("ok:mic_on");
            } else {
                snprintf(response, sizeof(response), "error:mic_start_failed:%d", rt);
                tcp_client_send_str(response);
            }
        }
    }
    else if (strncmp(data, "mic off", 7) == 0) {
        /* Stop microphone streaming */
        if (!mic_streaming_is_active()) {
            tcp_client_send_str("ok:mic_already_off");
        } else {
            mic_streaming_stop();
            tcp_client_send_str("ok:mic_off");
        }
    }
    else if (strncmp(data, "mic status", 10) == 0) {
        /* Get mic streaming status */
        uint32_t bytes_sent = 0, frames_sent = 0;
        mic_streaming_get_stats(&bytes_sent, &frames_sent);
        snprintf(response, sizeof(response), 
            "{\"active\":%s,\"bytes_sent\":%u,\"frames_sent\":%u}",
            mic_streaming_is_active() ? "true" : "false",
            bytes_sent, frames_sent);
        tcp_client_send_str(response);
    }
    else if (strncmp(data, "switch on", 9) == 0) {
        g_detection_active = true;
        /* Report to Tuya cloud */
        dp_obj_t dp = {.id = SWITCH_DP_ID, .type = PROP_BOOL, .value.dp_bool = true};
        tuya_iot_dp_obj_report(&client, NULL, &dp, 1, 0);
        tcp_client_send_str("ok:switch_on");
    }
    else if (strncmp(data, "switch off", 10) == 0) {
        g_detection_active = false;
        dp_obj_t dp = {.id = SWITCH_DP_ID, .type = PROP_BOOL, .value.dp_bool = false};
        tuya_iot_dp_obj_report(&client, NULL, &dp, 1, 0);
        tcp_client_send_str("ok:switch_off");
    }
    else if (strncmp(data, "mem", 3) == 0) {
        snprintf(response, sizeof(response), "heap:%d", tal_system_get_free_heap_size());
        tcp_client_send_str(response);
    }
    else if (strncmp(data, "reset", 5) == 0) {
        tcp_client_send_str("ok:resetting");
        tuya_iot_reset(tuya_iot_client_get());
    }
    else {
        snprintf(response, sizeof(response), "unknown_command:%.*s", len > 50 ? 50 : (int)len, data);
        tcp_client_send_str(response);
    }
}

/**
 * @brief Update speaker GPIO based on current volume
 * When volume is 0, disable speaker amplifier (mute)
 * When volume > 0, enable speaker amplifier
 */
static void update_speaker_gpio(uint8_t volume)
{
    TUYA_GPIO_LEVEL_E level = (volume > 0) ? TUYA_GPIO_LEVEL_HIGH : TUYA_GPIO_LEVEL_LOW;
    OPERATE_RET rt = tkl_gpio_write(SPEAKER_EN_GPIO, level);
    if (rt != OPRT_OK) {
        PR_ERR("Failed to update speaker GPIO: %d", rt);
    } else {
        PR_DEBUG("Speaker amplifier %s (volume=%d)", (volume > 0) ? "ENABLED" : "DISABLED/MUTED", volume);
    }
}

/**
 * @brief Play the alert audio when detection is triggered
 */
static void play_detection_alert(void)
{
    if (!g_audio_initialized) {
        PR_WARN("Audio not initialized, cannot play alert");
        return;
    }
    
    /* Check if volume is 0 (muted) - don't play audio */
    if (g_current_volume == 0) {
        PR_INFO("Volume is 0 (muted), not playing audio");
        return;
    }
    
    /* Stop any currently playing audio first */
    if (ai_audio_player_is_playing()) {
        PR_DEBUG("Stopping previous audio before playing new alert");
        ai_audio_player_stop();
    }
    
    PR_INFO("Playing detection alert audio (size=%d bytes)...", sizeof(alert_audio_data));
    
    /* Start the audio player with an ID */
    OPERATE_RET rt = ai_audio_player_start("detection_alert");
    if (rt != OPRT_OK) {
        PR_ERR("Failed to start audio player: %d", rt);
        return;
    }
    
    PR_DEBUG("Audio player started, writing data...");
    
    /* Write the prebuild audio data to the player */
    /* is_eof=1 means this is the complete audio file */
    rt = ai_audio_player_data_write("detection_alert", 
                                     (uint8_t *)alert_audio_data, 
                                     sizeof(alert_audio_data), 
                                     1);
    if (rt != OPRT_OK) {
        PR_ERR("Failed to write audio data: %d", rt);
    } else {
        PR_INFO("Audio data written successfully, playback should start");
    }
}

/**
 * @brief Stop any playing audio
 */
static void stop_detection_alert(void)
{
    if (!g_audio_initialized) {
        return;
    }
    
    if (ai_audio_player_is_playing()) {
        PR_INFO("Stopping detection alert audio...");
        ai_audio_player_stop();
    }
}

/**
 * @brief user defined log output api, in this demo, it will use uart0 as log-tx
 *
 * @param str log string
 * @return void
 */
void user_log_output_cb(const char *str)
{
    tkl_log_output(str);
}

/**
 * @brief user defined upgrade notify callback, it will notify device a OTA
 * request received
 *
 * @param client device info
 * @param upgrade the upgrade request info
 * @return void
 */
void user_upgrade_notify_on(tuya_iot_client_t *client, cJSON *upgrade)
{
    PR_INFO("----- Upgrade information -----");
    PR_INFO("OTA Channel: %d", cJSON_GetObjectItem(upgrade, "type")->valueint);
    PR_INFO("Version: %s", cJSON_GetObjectItem(upgrade, "version")->valuestring);
    PR_INFO("Size: %s", cJSON_GetObjectItem(upgrade, "size")->valuestring);
    PR_INFO("MD5: %s", cJSON_GetObjectItem(upgrade, "md5")->valuestring);
    PR_INFO("HMAC: %s", cJSON_GetObjectItem(upgrade, "hmac")->valuestring);
    PR_INFO("URL: %s", cJSON_GetObjectItem(upgrade, "url")->valuestring);
    PR_INFO("HTTPS URL: %s", cJSON_GetObjectItem(upgrade, "httpsUrl")->valuestring);
}

/**
 * @brief user defined event handler
 *
 * @param client device info
 * @param event the event info
 * @return void
 */
void user_event_handler_on(tuya_iot_client_t *client, tuya_event_msg_t *event)
{
    PR_DEBUG("Tuya Event ID:%d(%s)", event->id, EVENT_ID2STR(event->id));
    PR_INFO("Device Free heap %d", tal_system_get_free_heap_size());
    switch (event->id) {
    case TUYA_EVENT_BIND_START:
        PR_INFO("Device Bind Start!");
        break;

    /* Print the QRCode for Tuya APP bind */
    case TUYA_EVENT_DIRECT_MQTT_CONNECTED: {
        char buffer[255];
        sprintf(buffer, "https://smartapp.tuya.com/s/p?p=%s&uuid=%s&v=2.0", TUYA_PRODUCT_ID, license.uuid);
        PR_NOTICE("=== Device Binding URL ===");
        PR_NOTICE("Scan this QR code or open URL in browser to bind device:");
        PR_NOTICE("%s", buffer);
        PR_NOTICE("==========================");
#if defined(ENABLE_QRCODE) && (ENABLE_QRCODE == 1)
        PR_NOTICE("QR Code:");
        qrcode_string_output(buffer, user_log_output_cb, 0);
#else
        PR_WARN("QR Code display not enabled. Use URL above or rebuild with CONFIG_ENABLE_QRCODE=y");
#endif
    } break;


    /* MQTT with tuya cloud is connected, device online */
    case TUYA_EVENT_MQTT_CONNECTED:
        PR_INFO("Device MQTT Connected!");
        /* TODO: Report initial state after connection stabilizes */
        break;

    /* RECV upgrade request */
    case TUYA_EVENT_UPGRADE_NOTIFY:
        user_upgrade_notify_on(client, event->value.asJSON);
        break;

    /* Sync time with tuya Cloud */
    case TUYA_EVENT_TIMESTAMP_SYNC:
        PR_INFO("Sync timestamp:%d", event->value.asInteger);
        tal_time_set_posix(event->value.asInteger, 1);
        break;
    case TUYA_EVENT_RESET:
        PR_INFO("Device Reset:%d", event->value.asInteger);
        break;

    /* RECV OBJ DP */
    case TUYA_EVENT_DP_RECEIVE_OBJ: {
        dp_obj_recv_t *dpobj = event->value.dpobj;
        PR_DEBUG("SOC Rev DP Cmd t1:%d t2:%d CNT:%u", dpobj->cmd_tp, dpobj->dtt_tp, dpobj->dpscnt);
        if (dpobj->devid != NULL) {
            PR_DEBUG("devid.%s", dpobj->devid);
        }

        uint32_t index = 0;
        for (index = 0; index < dpobj->dpscnt; index++) {
            dp_obj_t *dp = dpobj->dps + index;
            PR_DEBUG("idx:%d dpid:%d type:%d ts:%u", index, dp->id, dp->type, dp->time_stamp);
            switch (dp->type) {
            case PROP_BOOL: {
                PR_DEBUG("bool value:%d", dp->value.dp_bool);
                /* Check if this is the detection switch DP (usually DP ID 1) */
                if (dp->id == SWITCH_DP_ID) {
                    BOOL_T new_state = dp->value.dp_bool;
                    BOOL_T state_changed = (new_state != g_detection_active);
                    
                    /* Update global state */
                    g_detection_active = new_state;
                    
                    /* Report the new state back to the cloud/app immediately */
                    tuya_iot_dp_obj_report(client, dpobj->devid, dp, 1, 0);

                    /* Only trigger actions if the state actually changed */
                    if (state_changed) {
                        if (g_detection_active) {
                            /* Detection ON - Play the alert audio */
                            PR_INFO("Object Detection: ACTIVATED - Playing alert");
                            play_detection_alert();
                        } else {
                            /* Detection OFF - Stop any playing audio */
                            PR_INFO("Object Detection: DEACTIVATED - Stopping audio");
                            stop_detection_alert();
                        }
                    } else {
                        PR_DEBUG("Switch state unchanged (%d), ignoring duplicate command", new_state);
                    }
                }
                break;
            }
            case PROP_VALUE: {
                PR_DEBUG("int value:%d", dp->value.dp_value);
                /* Check if this is the volume DP */
                if (dp->id == VOLUME_DP_ID) {
                    uint8_t volume = (uint8_t)dp->value.dp_value;
                    PR_INFO("Setting volume to: %d", volume);
                    
                    /* Update stored volume */
                    g_current_volume = volume;
                    
                    /* Set DAC gain */
                    ai_audio_set_volume(volume);
                    
                    /* Control speaker amplifier GPIO based on volume */
                    update_speaker_gpio(volume);
                    
                    /* Report the new volume back to the cloud/app */
                    tuya_iot_dp_obj_report(client, dpobj->devid, dp, 1, 0);
                }
                break;
            }
            case PROP_STR: {
                PR_DEBUG("str value:%s", dp->value.dp_str);
                break;
            }
            case PROP_ENUM: {
                PR_DEBUG("enum value:%u", dp->value.dp_enum);
                break;
            }
            case PROP_BITMAP: {
                PR_DEBUG("bits value:0x%X", dp->value.dp_bitmap);
                break;
            }
            default: {
                PR_ERR("idx:%d dpid:%d type:%d ts:%u is invalid", index, dp->id, dp->type, dp->time_stamp);
                break;
            }
            } // end of switch
        }
        
    } break;

    /* RECV RAW DP */
    case TUYA_EVENT_DP_RECEIVE_RAW: {
        dp_raw_recv_t *dpraw = event->value.dpraw;
        PR_DEBUG("SOC Rev DP Cmd t1:%d t2:%d", dpraw->cmd_tp, dpraw->dtt_tp);
        if (dpraw->devid != NULL) {
            PR_DEBUG("devid.%s", dpraw->devid);
        }

        uint32_t index = 0;
        dp_raw_t *dp = &dpraw->dp;
        PR_DEBUG("dpid:%d type:RAW len:%d data:", dp->id, dp->len);
        for (index = 0; index < dp->len; index++) {
            PR_DEBUG_RAW("%02x", dp->data[index]);
        }

        tuya_iot_dp_raw_report(client, dpraw->devid, &dpraw->dp, 3);

    } break;

        /* TBD.. add other event if necessary */

    default:
        break;
    }
}

/**
 * @brief user defined network check callback, it will check the network every
 * 1sec, in this demo it alwasy return ture due to it's a wired demo
 *
 * @return true
 * @return false
 */
bool user_network_check(void)
{
    netmgr_status_e status = NETMGR_LINK_DOWN;
    netmgr_conn_get(NETCONN_AUTO, NETCONN_CMD_STATUS, &status);
    return status == NETMGR_LINK_DOWN ? false : true;
}

void user_main(void)
{
    int rt = OPRT_OK;

    //! open iot development kit runtim init
    cJSON_InitHooks(&(cJSON_Hooks){.malloc_fn = tal_malloc, .free_fn = tal_free});
    tal_log_init(TAL_LOG_LEVEL_DEBUG, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    PR_NOTICE("Application information:");
    PR_NOTICE("Project name:        %s", PROJECT_NAME);
    PR_NOTICE("App version:         %s", PROJECT_VERSION);
    PR_NOTICE("Compile time:        %s", __DATE__);
    PR_NOTICE("TuyaOpen version:    %s", OPEN_VERSION);
    PR_NOTICE("TuyaOpen commit-id:  %s", OPEN_COMMIT);
    PR_NOTICE("Platform chip:       %s", PLATFORM_CHIP);
    PR_NOTICE("Platform board:      %s", PLATFORM_BOARD);
    PR_NOTICE("Platform commit-id:  %s", PLATFORM_COMMIT);

    tal_kv_init(&(tal_kv_cfg_t){
        .seed = "vmlkasdh93dlvlcy",
        .key = "dflfuap134ddlduq",
    });
    tal_sw_timer_init();
    tal_workq_init();

    /* Register board hardware (audio driver, button, LED) */
    PR_INFO("Registering board hardware...");
    rt = board_register_hardware();
    if (rt != OPRT_OK) {
        PR_ERR("Failed to register board hardware: %d", rt);
    } else {
        PR_INFO("Board hardware registered successfully");
    }

    /* Initialize the audio player */
    PR_INFO("Initializing audio player...");
    rt = ai_audio_player_init();
    if (rt != OPRT_OK) {
        PR_ERR("Failed to initialize audio player: %d", rt);
        g_audio_initialized = false;
    } else {
        PR_INFO("Audio player initialized successfully");
        
        /* Open the audio device to enable playback */
        TDL_AUDIO_HANDLE_T audio_hdl = NULL;
        rt = tdl_audio_find(AUDIO_CODEC_NAME, &audio_hdl);
        if (rt != OPRT_OK) {
            PR_ERR("Failed to find audio codec: %d", rt);
            g_audio_initialized = false;
        } else {
            rt = tdl_audio_open(audio_hdl, NULL);
            if (rt != OPRT_OK) {
                PR_ERR("Failed to open audio device: %d", rt);
                g_audio_initialized = false;
            } else {
                PR_INFO("Audio device opened successfully");
                g_audio_initialized = true;
                
                /* Enable speaker amplifier GPIO (GPIO39 on T5AI-CORE) */
                /* Speaker is active when GPIO is HIGH (polarity=LOW means LOW=mute) */
                PR_INFO("Enabling speaker amplifier (GPIO39)...");
                TUYA_GPIO_BASE_CFG_T spk_gpio_cfg = {
                    .direct = TUYA_GPIO_OUTPUT,
                    .mode = TUYA_GPIO_PUSH_PULL,
                    .level = TUYA_GPIO_LEVEL_HIGH  /* HIGH = speaker enabled */
                };
                rt = tkl_gpio_init(TUYA_GPIO_NUM_39, &spk_gpio_cfg);
                if (rt != OPRT_OK) {
                    PR_ERR("Failed to init speaker GPIO: %d", rt);
                } else {
                    rt = tkl_gpio_write(TUYA_GPIO_NUM_39, TUYA_GPIO_LEVEL_HIGH);
                    if (rt != OPRT_OK) {
                        PR_ERR("Failed to enable speaker GPIO: %d", rt);
                    } else {
                        PR_INFO("Speaker amplifier enabled (GPIO39=HIGH)");
                    }
                }
                
                /* Set default volume */
                PR_INFO("Setting default volume to %d", DEFAULT_VOLUME);
                g_current_volume = DEFAULT_VOLUME;
                rt = ai_audio_set_volume(DEFAULT_VOLUME);
                if (rt != OPRT_OK) {
                    PR_ERR("Failed to set volume: %d", rt);
                } else {
                    PR_INFO("Volume set successfully");
                }
            }
        }
    }

    /* Initialize microphone streaming (for web app audio) */
    PR_INFO("Initializing microphone streaming...");
    rt = mic_streaming_init();
    if (rt != OPRT_OK) {
        PR_ERR("Failed to initialize mic streaming: %d", rt);
    } else {
        PR_INFO("Microphone streaming initialized (standby mode)");
    }

#if !defined(PLATFORM_UBUNTU) || (PLATFORM_UBUNTU == 0)
    tal_cli_init();
    tuya_authorize_init();
    tuya_app_cli_init();
#endif

    reset_netconfig_start();

    if (OPRT_OK != tuya_authorize_read(&license)) {
        license.uuid = TUYA_OPENSDK_UUID;
        license.authkey = TUYA_OPENSDK_AUTHKEY;
        PR_WARN("Replace the TUYA_OPENSDK_UUID and TUYA_OPENSDK_AUTHKEY contents, otherwise the demo cannot work.\n \
                Visit https://platform.tuya.com/purchase/index?type=6 to get the open-sdk uuid and authkey.");
    }
    // PR_DEBUG("uuid %s, authkey %s", license.uuid, license.authkey);
    /* Initialize Tuya device configuration */
    rt = tuya_iot_init(&client, &(const tuya_iot_config_t){
                                    .software_ver = PROJECT_VERSION,
                                    .productkey = TUYA_PRODUCT_ID,
                                    .uuid = license.uuid,
                                    .authkey = license.authkey,
                                    .event_handler = user_event_handler_on,
                                    .network_check = user_network_check,
                                });
    assert(rt == OPRT_OK);

#if defined(ENABLE_LIBLWIP) && (ENABLE_LIBLWIP == 1)
    TUYA_LwIP_Init();
#endif

    // network init
    netmgr_type_e type = 0;
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    type |= NETCONN_WIFI;
#endif
#if defined(ENABLE_WIRED) && (ENABLE_WIRED == 1)
    type |= NETCONN_WIRED;
#endif
#if defined(ENABLE_CELLULAR) && (ENABLE_CELLULAR == 1)
    type |= NETCONN_CELLULAR;
#endif
    netmgr_init(type);

#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
    netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_NETCFG, &(netcfg_args_t){.type = NETCFG_TUYA_BLE | NETCFG_TUYA_WIFI_AP});
#endif

    PR_DEBUG("tuya_iot_init success");
    
    /* Print Device Binding Information at startup */
    {
        char binding_url[255];
        sprintf(binding_url, "https://smartapp.tuya.com/s/p?p=%s&uuid=%s&v=2.0", TUYA_PRODUCT_ID, license.uuid);
        PR_NOTICE("============================================");
        PR_NOTICE("     DEVICE BINDING INFORMATION");
        PR_NOTICE("============================================");
        PR_NOTICE("Product ID: %s", TUYA_PRODUCT_ID);
        PR_NOTICE("UUID: %s", license.uuid);
        PR_NOTICE("Binding URL:");
        PR_NOTICE("%s", binding_url);
        PR_NOTICE("============================================");
#if defined(ENABLE_QRCODE) && (ENABLE_QRCODE == 1)
        PR_NOTICE("QR Code (scan with Tuya Smart Life app):");
        qrcode_string_output(binding_url, user_log_output_cb, 0);
#else
        PR_NOTICE("(QR code not compiled - use URL above)");
#endif
        PR_NOTICE("============================================");
    }
    
    /* Start tuya iot task */
    tuya_iot_start(&client);

    reset_netconfig_check();

    /* Initialize BLE configuration handler */
    ble_config_init();

    /* Initialize TCP client for web app communication */
    /* First try to load saved settings from KV storage */
    char tcp_host[64] = "";
    uint16_t tcp_port = TCP_SERVER_PORT;
    char tcp_token[64] = "";
    
    if (ble_config_load_tcp_settings(tcp_host, &tcp_port, tcp_token) == OPRT_OK && tcp_host[0]) {
        PR_NOTICE("============================================");
        PR_NOTICE("     WEB APP CONNECTION (SAVED)");
        PR_NOTICE("============================================");
        PR_NOTICE("TCP Server: %s:%d", tcp_host, tcp_port);
        PR_NOTICE("(Settings loaded from flash storage)");
        PR_NOTICE("Configure: https://ble-config-web.vercel.app");
        PR_NOTICE("============================================");
    } else {
        /* Use compile-time defaults */
        strncpy(tcp_host, TCP_SERVER_HOST, sizeof(tcp_host) - 1);
        tcp_port = TCP_SERVER_PORT;
        
        PR_NOTICE("============================================");
        PR_NOTICE("     WEB APP CONNECTION (DEFAULT)");
        PR_NOTICE("============================================");
        PR_NOTICE("TCP Server: %s:%d", tcp_host, tcp_port);
        PR_NOTICE("(Using compile-time defaults from .env)");
        PR_NOTICE("Configure: https://ble-config-web.vercel.app");
        PR_NOTICE("============================================");
    }
    
    if (tcp_client_init(tcp_host, tcp_port, tcp_message_callback) == OPRT_OK) {
        tcp_client_start();
        PR_INFO("TCP client started - will connect to web app server");
    } else {
        PR_ERR("Failed to initialize TCP client");
    }
    /* Note: RTSP tunnel feature disabled - see plan/TTS_STREAMING_PLAN.md for audio streaming */
#if RTSP_TUNNEL_ENABLED
    /* RTSP tunnel auto-start if enabled at compile time */
    {
        rtsp_tunnel_config_t cfg = {0};
        strncpy(cfg.camera_host, RTSP_CAMERA_HOST, sizeof(cfg.camera_host) - 1);
        cfg.camera_port = RTSP_CAMERA_PORT;
        strncpy(cfg.vps_host, RTSP_VPS_HOST, sizeof(cfg.vps_host) - 1);
        cfg.vps_port = RTSP_VPS_PORT;
        
        if (rtsp_tunnel_init(&cfg, NULL) == OPRT_OK && rtsp_tunnel_start() == OPRT_OK) {
            g_rtsp_tunnel_active = true;
            PR_NOTICE("RTSP tunnel started automatically");
        } else {
            PR_ERR("Failed to auto-start RTSP tunnel");
        }
    }
#endif

    for (;;) {
        /* Loop to receive packets, and handles client keepalive */
        tuya_iot_yield(&client);
    }
}

/**
 * @brief main
 *
 * @param argc
 * @param argv
 * @return void
 */
#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    user_main();
}
#else

/* Tuya thread handle */
static THREAD_HANDLE ty_app_thread = NULL;

/**
 * @brief  task thread
 *
 * @param[in] arg:Parameters when creating a task
 * @return none
 */
static void tuya_app_thread(void *arg)
{
    user_main();

    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thrd_param = {4096, 4, "tuya_app_main"};
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}
#endif
