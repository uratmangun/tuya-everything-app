/**
 * @file ble_config.c
 * @brief BLE Configuration Handler for Local Device Configuration
 * 
 * This module provides:
 * 1. Persistent KV storage for TCP server settings
 * 2. WiFi direct connection capability
 * 3. BLE channel handler for receiving config commands
 *
 * Web Bluetooth Page: https://ble-config-web.vercel.app
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#include "ble_config.h"
#include "tal_api.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

/* WiFi direct connect API */
#include "tkl_wifi.h"
#include "tal_wifi.h"

/* BLE channel for receiving config commands */
#include "ble_channel.h"
#include "ble_mgr.h"
#include "ble_protocol.h"

/***********************************************************
 * WiFi Direct Connection
 ***********************************************************/

/**
 * @brief Save WiFi credentials to KV storage
 */
static OPERATE_RET save_wifi_credentials(const char *ssid, const char *password)
{
    OPERATE_RET rt;
    
    rt = tal_kv_set("wifi_ssid", (const uint8_t *)ssid, strlen(ssid) + 1);
    if (rt != OPRT_OK) return rt;
    
    rt = tal_kv_set("wifi_passwd", (const uint8_t *)password, strlen(password) + 1);
    return rt;
}

/**
 * @brief Connect to WiFi directly using tkl_wifi_station_connect
 * 
 * This bypasses the Tuya cloud pairing and connects directly to the specified
 * WiFi network. Use this when you need to change WiFi without going through
 * the Tuya Smart Life app flow.
 */
OPERATE_RET ble_config_wifi_connect(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0) {
        return OPRT_INVALID_PARM;
    }
    
    PR_NOTICE("============================================");
    PR_NOTICE("     CONNECTING TO WIFI DIRECTLY");
    PR_NOTICE("============================================");
    PR_NOTICE("SSID: %s", ssid);
    PR_NOTICE("Password length: %d", password ? strlen(password) : 0);
    
    /* Save credentials first */
    save_wifi_credentials(ssid, password ? password : "");
    
    /* Connect directly using TKL API */
    OPERATE_RET rt = tkl_wifi_station_connect((const SCHAR_T *)ssid, (const SCHAR_T *)password);
    
    if (rt == OPRT_OK) {
        PR_NOTICE("WiFi connection initiated successfully!");
        PR_NOTICE("Device will obtain IP via DHCP...");
    } else {
        PR_ERR("WiFi connection failed: %d", rt);
    }
    
    return rt;
}

/***********************************************************
 * BLE Configuration Channel Handler
 * 
 * This handles commands received via BLE from Web Bluetooth.
 * Commands are JSON formatted:
 * - {"cmd":"set_tcp","host":"...","port":5000,"token":"..."}
 * - {"cmd":"set_wifi","ssid":"...","password":"..."}
 * - {"cmd":"get_status"}
 * - {"cmd":"reboot"}
 ***********************************************************/

/* Channel type for our config handler - use 0 since BLE_CHANNEL_MAX is only 2 */
#define BLE_CHANNEL_CONFIG 0

static void ble_config_channel_handler(void *data, void *user_data)
{
    if (!data) return;
    
    char *json_str = (char *)data;
    PR_INFO("BLE Config received: %s", json_str);
    
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        PR_ERR("BLE config: invalid JSON");
        return;
    }
    
    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cmd || !cJSON_IsString(cmd)) {
        PR_ERR("BLE config: missing cmd");
        cJSON_Delete(root);
        return;
    }
    
    /* Handle set_tcp command */
    if (strcmp(cmd->valuestring, "set_tcp") == 0) {
        cJSON *host = cJSON_GetObjectItem(root, "host");
        cJSON *port = cJSON_GetObjectItem(root, "port");
        cJSON *token = cJSON_GetObjectItem(root, "token");
        
        if (host && cJSON_IsString(host)) {
            uint16_t port_val = 5000;
            if (port && cJSON_IsNumber(port)) {
                port_val = (uint16_t)port->valueint;
            }
            
            const char *token_val = "";
            if (token && cJSON_IsString(token)) {
                token_val = token->valuestring;
            }
            
            ble_config_save_tcp_settings(host->valuestring, port_val, token_val);
            PR_NOTICE("BLE config: TCP settings saved - %s:%d", host->valuestring, port_val);
            
            /* Send response via BLE */
            uint8_t resp[] = {0x00, 0x00, 0x00, BLE_CHANNEL_CONFIG, 0x00}; /* Success */
            tuya_ble_send(FRM_UPLINK_TRANSPARENT_REQ, 0, resp, sizeof(resp));
        }
    }
    /* Handle set_wifi command */
    else if (strcmp(cmd->valuestring, "set_wifi") == 0) {
        cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
        cJSON *password = cJSON_GetObjectItem(root, "password");
        
        if (ssid && cJSON_IsString(ssid)) {
            const char *pw = "";
            if (password && cJSON_IsString(password)) {
                pw = password->valuestring;
            }
            
            OPERATE_RET rt = ble_config_wifi_connect(ssid->valuestring, pw);
            
            /* Send response */
            uint8_t resp[] = {0x00, 0x00, 0x00, BLE_CHANNEL_CONFIG, (uint8_t)rt};
            tuya_ble_send(FRM_UPLINK_TRANSPARENT_REQ, 0, resp, sizeof(resp));
        }
    }
    /* Handle get_status command */
    else if (strcmp(cmd->valuestring, "get_status") == 0) {
        int heap = tal_system_get_free_heap_size();
        
        /* Build status JSON response */
        cJSON *status = cJSON_CreateObject();
        cJSON_AddStringToObject(status, "type", "status");
        cJSON_AddNumberToObject(status, "heap", heap);
        
        char *status_str = cJSON_PrintUnformatted(status);
        if (status_str) {
            PR_NOTICE("BLE config: Status - %s", status_str);
            /* Would send via BLE notify here */
            tal_free(status_str);
        }
        cJSON_Delete(status);
    }
    /* Handle wifi_scan command */
    else if (strcmp(cmd->valuestring, "wifi_scan") == 0) {
        PR_NOTICE("BLE config: WiFi scan requested");
        
        /* Check if WiFi is connected - scanning will disrupt connection */
        WF_STATION_STAT_E wifi_stat = WSS_IDLE;
        tal_wifi_station_get_status(&wifi_stat);
        
        if (wifi_stat == WSS_GOT_IP) {
            PR_WARN("WiFi is connected - scan will disrupt connection!");
            /* Send error response */
            const char *err_json = "{\"type\":\"error\",\"msg\":\"WiFi connected. Disconnect first to scan.\"}";
            size_t err_len = strlen(err_json);
            uint8_t *resp = tal_malloc(4 + err_len);
            if (resp) {
                resp[0] = 0x00; resp[1] = 0x00; resp[2] = 0x00; resp[3] = BLE_CHANNEL_CONFIG;
                memcpy(resp + 4, err_json, err_len);
                tuya_ble_send(FRM_UPLINK_TRANSPARENT_REQ, 0, resp, 4 + err_len);
                tal_free(resp);
            }
            cJSON_Delete(root);
            return;
        }
        
        AP_IF_S *ap_info = NULL;
        uint32_t ap_count = 0;
        
        OPERATE_RET rt = tal_wifi_all_ap_scan(&ap_info, &ap_count);
        if (rt == OPRT_OK && ap_info) {
            PR_NOTICE("WiFi scan found %d networks", ap_count);
            
            /* Build JSON response with WiFi list */
            cJSON *resp_json = cJSON_CreateObject();
            cJSON_AddStringToObject(resp_json, "type", "wifi_list");
            cJSON *networks = cJSON_CreateArray();
            
            /* Limit to 10 networks to fit in BLE packet */
            uint32_t max_networks = (ap_count > 10) ? 10 : ap_count;
            for (uint32_t i = 0; i < max_networks; i++) {
                if (strlen((char*)ap_info[i].ssid) > 0) {  /* Skip hidden networks */
                    cJSON *net = cJSON_CreateObject();
                    cJSON_AddStringToObject(net, "ssid", (char*)ap_info[i].ssid);
                    cJSON_AddNumberToObject(net, "rssi", ap_info[i].rssi);
                    cJSON_AddNumberToObject(net, "ch", ap_info[i].channel);
                    cJSON_AddItemToArray(networks, net);
                }
            }
            cJSON_AddItemToObject(resp_json, "networks", networks);
            
            char *json_str = cJSON_PrintUnformatted(resp_json);
            if (json_str) {
                PR_NOTICE("Sending WiFi list via BLE: %d bytes", strlen(json_str));
                
                /* Send via BLE transparent channel */
                size_t json_len = strlen(json_str);
                uint8_t *resp = tal_malloc(4 + json_len);
                if (resp) {
                    resp[0] = 0x00;  /* flags low */
                    resp[1] = 0x00;  /* flags high */
                    resp[2] = 0x00;  /* channel high */
                    resp[3] = BLE_CHANNEL_CONFIG;  /* channel low */
                    memcpy(resp + 4, json_str, json_len);
                    tuya_ble_send(FRM_UPLINK_TRANSPARENT_REQ, 0, resp, 4 + json_len);
                    tal_free(resp);
                }
                tal_free(json_str);
            }
            cJSON_Delete(resp_json);
            tal_wifi_release_ap(ap_info);
        } else {
            PR_ERR("WiFi scan failed: %d", rt);
        }
    }
    /* Handle wifi_disconnect command */
    else if (strcmp(cmd->valuestring, "wifi_disconnect") == 0) {
        PR_NOTICE("BLE config: WiFi disconnect requested");
        
        /* Use netmgr to properly close connection and stop auto-reconnect */
        extern OPERATE_RET netmgr_conn_set(int type, int cmd, void *param);
        netmgr_conn_set(1, 6, NULL);  /* NETCONN_WIFI=1, NETCONN_CMD_CLOSE=6 */
        
        const char *ack_json = "{\"type\":\"ack\",\"msg\":\"WiFi disconnected\"}";
        size_t ack_len = strlen(ack_json);
        uint8_t *resp = tal_malloc(4 + ack_len);
        if (resp) {
            resp[0] = 0x00; resp[1] = 0x00; resp[2] = 0x00; resp[3] = BLE_CHANNEL_CONFIG;
            memcpy(resp + 4, ack_json, ack_len);
            tuya_ble_send(FRM_UPLINK_TRANSPARENT_REQ, 0, resp, 4 + ack_len);
            tal_free(resp);
        }
    }
    /* Handle reboot command */
    else if (strcmp(cmd->valuestring, "reboot") == 0) {
        PR_NOTICE("BLE config: Reboot requested");
        
        /* Send acknowledgment first */
        uint8_t resp[] = {0x00, 0x00, 0x00, BLE_CHANNEL_CONFIG, 0x00};
        tuya_ble_send(FRM_UPLINK_TRANSPARENT_REQ, 0, resp, sizeof(resp));
        
        /* Wait a bit then reboot */
        tal_system_sleep(500);
        tal_system_reset();
    }
    
    cJSON_Delete(root);
}

/***********************************************************
 * KV Storage Functions
 ***********************************************************/
OPERATE_RET ble_config_load_tcp_settings(char *host, uint16_t *port, char *token)
{
    OPERATE_RET rt;
    uint8_t *value = NULL;
    size_t len;
    
    if (!host || !port || !token) {
        return OPRT_INVALID_PARM;
    }
    
    /* Initialize with defaults */
    host[0] = '\0';
    token[0] = '\0';
    *port = 5000;
    
    /* Load host */
    rt = tal_kv_get(KV_TCP_SERVER_HOST, &value, &len);
    if (rt != OPRT_OK || !value) {
        PR_DEBUG("No saved TCP host found");
        return OPRT_NOT_FOUND;
    }
    if (len > 0 && len < 64) {
        memcpy(host, value, len);
        host[len] = '\0';
    }
    tal_kv_free(value);
    value = NULL;
    
    /* Load port */
    rt = tal_kv_get(KV_TCP_SERVER_PORT, &value, &len);
    if (rt == OPRT_OK && value && len == sizeof(uint16_t)) {
        *port = *(uint16_t *)value;
    }
    if (value) {
        tal_kv_free(value);
        value = NULL;
    }
    
    /* Load token */
    rt = tal_kv_get(KV_TCP_AUTH_TOKEN, &value, &len);
    if (rt == OPRT_OK && value && len > 0 && len < 64) {
        memcpy(token, value, len);
        token[len] = '\0';
    } else {
        strcpy(token, "devkit-secret-token");
    }
    if (value) {
        tal_kv_free(value);
        value = NULL;
    }
    
    return OPRT_OK;
}

OPERATE_RET ble_config_save_tcp_settings(const char *host, uint16_t port, const char *token)
{
    OPERATE_RET rt;
    
    if (!host || !token) {
        return OPRT_INVALID_PARM;
    }
    
    PR_INFO("Saving TCP settings: host=%s, port=%d, token_len=%d", host, port, strlen(token));
    
    rt = tal_kv_set(KV_TCP_SERVER_HOST, (const uint8_t *)host, strlen(host) + 1);
    if (rt != OPRT_OK) {
        PR_ERR("Failed to save host: %d", rt);
        return rt;
    }
    
    rt = tal_kv_set(KV_TCP_SERVER_PORT, (const uint8_t *)&port, sizeof(uint16_t));
    if (rt != OPRT_OK) {
        PR_ERR("Failed to save port: %d", rt);
        return rt;
    }
    
    rt = tal_kv_set(KV_TCP_AUTH_TOKEN, (const uint8_t *)token, strlen(token) + 1);
    if (rt != OPRT_OK) {
        PR_ERR("Failed to save token: %d", rt);
        return rt;
    }
    
    PR_INFO("TCP settings saved successfully");
    return OPRT_OK;
}

/***********************************************************
 * Public API
 ***********************************************************/
OPERATE_RET ble_config_init(void)
{
    PR_NOTICE("============================================");
    PR_NOTICE("     BLE CONFIGURATION ENABLED");
    PR_NOTICE("============================================");
    PR_NOTICE("To configure this device:");
    PR_NOTICE("1. Open https://ble-config-web.vercel.app");
    PR_NOTICE("2. Connect via Bluetooth");
    PR_NOTICE("3. Configure WiFi and TCP settings");
    PR_NOTICE("============================================");
    
    /* Register BLE channel for config commands 
     * This uses the Tuya BLE channel infrastructure to receive
     * transparent data from BLE connections.
     */
    int rt = ble_channel_add(BLE_CHANNEL_CONFIG, ble_config_channel_handler, NULL);
    if (rt != OPRT_OK) {
        PR_WARN("BLE config channel registration: %d (BLE may not be ready yet)", rt);
        /* Not fatal - the channel will be available once BLE is fully initialized */
    } else {
        PR_INFO("BLE config channel registered successfully");
    }
    
    return OPRT_OK;
}
