/**
 * @file tcp_client.h
 * @brief TCP Client for communication with Web App Server
 * 
 * This module provides TCP connectivity to the local web server,
 * allowing bidirectional communication between the DevKit and web UI.
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#ifndef __TCP_CLIENT_H__
#define __TCP_CLIENT_H__

#include "tuya_cloud_types.h"

/**
 * @brief Callback for received messages
 * @param data Message data
 * @param len Message length
 */
typedef void (*tcp_client_recv_cb_t)(const char *data, uint32_t len);

/**
 * @brief Initialize TCP client
 * @param host Server IP address (e.g., "192.168.1.100")
 * @param port Server port (e.g., 5000)
 * @param recv_cb Callback for received messages
 * @return OPRT_OK on success
 */
OPERATE_RET tcp_client_init(const char *host, uint16_t port, tcp_client_recv_cb_t recv_cb);

/**
 * @brief Start TCP client (connects to server)
 * @return OPRT_OK on success
 */
OPERATE_RET tcp_client_start(void);

/**
 * @brief Stop TCP client
 */
void tcp_client_stop(void);

/**
 * @brief Check if connected to server
 * @return true if connected
 */
bool tcp_client_is_connected(void);

/**
 * @brief Send message to server
 * @param data Message data
 * @param len Message length
 * @return OPRT_OK on success
 */
OPERATE_RET tcp_client_send(const char *data, uint32_t len);

/**
 * @brief Send string message to server
 * @param str Null-terminated string
 * @return OPRT_OK on success
 */
OPERATE_RET tcp_client_send_str(const char *str);

#endif /* __TCP_CLIENT_H__ */
