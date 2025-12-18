/**
 * @file ble_config.h
 * @brief BLE Configuration Handler for Web Bluetooth
 * 
 * Provides BLE-based configuration for TCP server and WiFi settings.
 * Works with the Web Bluetooth configuration page at https://ble-config-web.vercel.app
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#ifndef __BLE_CONFIG_H__
#define __BLE_CONFIG_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* KV Storage Keys for TCP Configuration */
#define KV_TCP_SERVER_HOST   "tcp_srv_host"
#define KV_TCP_SERVER_PORT   "tcp_srv_port"
#define KV_TCP_AUTH_TOKEN    "tcp_auth_token"

/**
 * @brief Initialize BLE configuration handler
 * 
 * Registers a BLE user session to handle config commands from Web Bluetooth.
 * 
 * @return OPRT_OK on success
 */
OPERATE_RET ble_config_init(void);

/**
 * @brief Load TCP settings from KV storage
 * 
 * @param[out] host Buffer for host address (min 64 bytes)
 * @param[out] port Pointer to store port number
 * @param[out] token Buffer for auth token (min 64 bytes)
 * @return OPRT_OK if settings exist and were loaded
 *         OPRT_NOT_FOUND if no saved settings exist
 */
OPERATE_RET ble_config_load_tcp_settings(char *host, uint16_t *port, char *token);

/**
 * @brief Save TCP settings to KV storage
 * 
 * @param[in] host TCP server host address
 * @param[in] port TCP server port
 * @param[in] token Authentication token
 * @return OPRT_OK on success
 */
OPERATE_RET ble_config_save_tcp_settings(const char *host, uint16_t port, const char *token);

/**
 * @brief Connect to WiFi network directly
 * 
 * This function connects to the specified WiFi network using tkl_wifi_station_connect.
 * It bypasses the Tuya pairing flow and connects directly.
 * 
 * @param[in] ssid WiFi network SSID
 * @param[in] password WiFi password (can be NULL for open networks)
 * @return OPRT_OK if connection initiated successfully
 */
OPERATE_RET ble_config_wifi_connect(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_CONFIG_H__ */
