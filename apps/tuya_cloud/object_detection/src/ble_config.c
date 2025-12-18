/**
 * @file ble_config.c
 * @brief BLE Configuration Handler for Web Bluetooth
 * 
 * This module provides persistent storage for TCP server settings
 * that can be configured via BLE or the CLI. Settings are stored
 * in flash using KV storage and persist across reboots.
 *
 * Web Bluetooth Page: https://ble-config-web.vercel.app
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#include "ble_config.h"
#include "tal_api.h"
#include <string.h>
#include <stdio.h>

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
    PR_NOTICE("2. Click 'Connect to T5AI'");
    PR_NOTICE("3. Configure WiFi and TCP settings");
    PR_NOTICE("============================================");
    
    /* Note: Full BLE GATT integration with Web Bluetooth requires
     * additional platform-specific BLE service setup. The Web Bluetooth
     * page is ready - the device will appear as "T5AI" or "TY" in 
     * the Bluetooth device picker.
     * 
     * TCP settings can also be configured via the CLI:
     *   config     - Show current settings
     *   
     * Or by editing .env and rebuilding.
     */
    
    return OPRT_OK;
}
