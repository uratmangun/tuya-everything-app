/**
 * @file tcp_client.c
 * @brief TCP Client implementation for Web App communication
 * 
 * Connects to a local server and exchanges messages with the web UI.
 * Protocol: [LENGTH:4 bytes LE][DATA:N bytes]
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#include "tcp_client.h"
#include "tal_api.h"
#include "tal_network.h"
#include <string.h>

/***********************************************************
************************macro define************************
***********************************************************/
#define TCP_RECV_BUF_SIZE       2048
#define TCP_RECONNECT_DELAY_MS  5000
#define TCP_RECV_TIMEOUT_MS     5000   /* 5 seconds - long enough to not spam, short enough to detect disconnect */

/***********************************************************
***********************typedef define***********************
***********************************************************/
typedef struct {
    char host[64];
    uint16_t port;
    int socket_fd;
    bool connected;
    bool running;
    THREAD_HANDLE thread;
    MUTEX_HANDLE mutex;
    tcp_client_recv_cb_t recv_cb;
    uint8_t recv_buf[TCP_RECV_BUF_SIZE];
} tcp_client_ctx_t;

/***********************************************************
***********************variable define**********************
***********************************************************/
static tcp_client_ctx_t g_ctx = {0};

/***********************************************************
***********************function define**********************
***********************************************************/

/**
 * @brief Connect to server
 */
static OPERATE_RET connect_to_server(void)
{
    OPERATE_RET rt = OPRT_OK;
    
    /* Create TCP socket */
    g_ctx.socket_fd = tal_net_socket_create(PROTOCOL_TCP);
    if (g_ctx.socket_fd < 0) {
        PR_ERR("Failed to create socket");
        return OPRT_SOCK_ERR;
    }
    
    /* Set socket timeout */
    tal_net_set_timeout(g_ctx.socket_fd, TCP_RECV_TIMEOUT_MS, TRANS_RECV);
    
    /* Resolve host to IP */
    TUYA_IP_ADDR_T addr = tal_net_str2addr(g_ctx.host);
    if (addr == 0) {
        PR_ERR("Failed to resolve host: %s", g_ctx.host);
        tal_net_close(g_ctx.socket_fd);
        g_ctx.socket_fd = -1;
        return OPRT_SOCK_ERR;
    }
    
    /* Connect to server */
    rt = tal_net_connect(g_ctx.socket_fd, addr, g_ctx.port);
    if (rt != OPRT_OK) {
        PR_ERR("Failed to connect to %s:%d (err: %d)", g_ctx.host, g_ctx.port, rt);
        tal_net_close(g_ctx.socket_fd);
        g_ctx.socket_fd = -1;
        return rt;
    }
    
    g_ctx.connected = true;
    PR_NOTICE("Connected to server %s:%d", g_ctx.host, g_ctx.port);
    
    /* Send authentication message */
#ifdef TCP_AUTH_TOKEN
    char auth_msg[128];
    snprintf(auth_msg, sizeof(auth_msg), "auth:%s", TCP_AUTH_TOKEN);
    tcp_client_send_str(auth_msg);
    PR_INFO("Sent authentication token");
#else
    tcp_client_send_str("auth:devkit-secret-token");
    PR_WARN("Using default auth token - please set TCP_AUTH_TOKEN in .env");
#endif
    
    return OPRT_OK;
}

/**
 * @brief Disconnect from server
 */
static void disconnect_from_server(void)
{
    if (g_ctx.socket_fd >= 0) {
        tal_net_close(g_ctx.socket_fd);
        g_ctx.socket_fd = -1;
    }
    g_ctx.connected = false;
}

/**
 * @brief Receiver task
 */
static void tcp_receiver_task(void *arg)
{
    int recv_len;
    uint8_t header[4];
    uint32_t msg_len;
    
    PR_INFO("TCP client task started");
    
    while (g_ctx.running) {
        /* Reconnect if disconnected */
        if (!g_ctx.connected) {
            PR_INFO("Attempting to connect to %s:%d...", g_ctx.host, g_ctx.port);
            
            if (connect_to_server() != OPRT_OK) {
                tal_system_sleep(TCP_RECONNECT_DELAY_MS);
                continue;
            }
        }
        
        /* Try to read message header (4 bytes = length) */
        recv_len = tal_net_recv(g_ctx.socket_fd, header, 4);
        
        if (recv_len < 0) {
            /* Check specific error */
            int err = tal_net_get_errno();
            
            /* Timeout or would-block is normal, just continue */
            if (err == UNW_ETIMEDOUT || err == UNW_EAGAIN || err == UNW_EWOULDBLOCK || err == 0) {
                /* Debug: Log occasionally to confirm loop is running */
                static int timeout_count = 0;
                if (++timeout_count % 100 == 0) {
                    PR_DEBUG("TCP recv timeout (count=%d)", timeout_count);
                }
                continue;
            }
            
            /* Real error - disconnect and retry */
            PR_WARN("Recv error (errno: %d), reconnecting...", err);
            disconnect_from_server();
            continue;
        }
        
        if (recv_len == 0) {
            /* recv_len == 0 can mean:
             * 1. Connection closed by peer (real disconnect)
             * 2. No data available yet (some systems)
             * We check socket state to be sure */
            int err = tal_net_get_errno();
            if (err == UNW_ETIMEDOUT || err == UNW_EAGAIN || err == UNW_EWOULDBLOCK || err == 0) {
                /* Not a real disconnect, just no data */
                continue;
            }
            PR_WARN("Server closed connection (recv=0, errno=%d), reconnecting...", err);
            disconnect_from_server();
            continue;
        }
        
        PR_DEBUG("TCP recv header: len=%d, bytes: %02X %02X %02X %02X", 
                 recv_len, header[0], header[1], header[2], header[3]);
        
        if (recv_len < 4) {
            /* Incomplete header, wait for more data */
            PR_WARN("Incomplete header received: %d bytes", recv_len);
            continue;
        }
        
        /* Parse message length (little-endian) */
        msg_len = header[0] | (header[1] << 8) | (header[2] << 16) | (header[3] << 24);
        
        if (msg_len == 0 || msg_len > TCP_RECV_BUF_SIZE - 1) {
            PR_WARN("Invalid message length: %u", msg_len);
            continue;
        }
        
        PR_DEBUG("TCP expecting %u bytes of message body", msg_len);
        
        /* Read message body */
        recv_len = tal_net_recv(g_ctx.socket_fd, g_ctx.recv_buf, msg_len);
        
        if (recv_len <= 0) {
            PR_WARN("Failed to read message body (recv_len=%d)", recv_len);
            disconnect_from_server();
            continue;
        }
        
        /* Null-terminate for string handling */
        g_ctx.recv_buf[recv_len] = '\0';
        
        PR_INFO("Received from server: %s", g_ctx.recv_buf);
        
        /* Call callback */
        if (g_ctx.recv_cb) {
            g_ctx.recv_cb((const char *)g_ctx.recv_buf, recv_len);
        }
    }
    
    disconnect_from_server();
    PR_INFO("TCP client task stopped");
}

OPERATE_RET tcp_client_init(const char *host, uint16_t port, tcp_client_recv_cb_t recv_cb)
{
    OPERATE_RET rt = OPRT_OK;
    
    memset(&g_ctx, 0, sizeof(g_ctx));
    
    strncpy(g_ctx.host, host, sizeof(g_ctx.host) - 1);
    g_ctx.port = port;
    g_ctx.socket_fd = -1;
    g_ctx.recv_cb = recv_cb;
    
    TUYA_CALL_ERR_RETURN(tal_mutex_create_init(&g_ctx.mutex));
    (void)rt;  /* Suppress unused variable warning from macro */
    
    PR_INFO("TCP client initialized: %s:%d", host, port);
    
    return OPRT_OK;
}

OPERATE_RET tcp_client_start(void)
{
    OPERATE_RET rt = OPRT_OK;
    
    if (g_ctx.running) {
        PR_WARN("TCP client already running");
        return OPRT_OK;
    }
    
    g_ctx.running = true;
    
    THREAD_CFG_T cfg = {
        .stackDepth = 4096,
        .priority = THREAD_PRIO_3,
        .thrdname = "tcp_client"
    };
    
    rt = tal_thread_create_and_start(&g_ctx.thread, NULL, NULL,
                                      tcp_receiver_task, NULL, &cfg);
    
    if (rt != OPRT_OK) {
        PR_ERR("Failed to create TCP client thread: %d", rt);
        g_ctx.running = false;
        return rt;
    }
    
    PR_INFO("TCP client started");
    return OPRT_OK;
}

void tcp_client_stop(void)
{
    g_ctx.running = false;
    
    disconnect_from_server();
    
    if (g_ctx.thread) {
        tal_thread_delete(g_ctx.thread);
        g_ctx.thread = NULL;
    }
    
    PR_INFO("TCP client stopped");
}

bool tcp_client_is_connected(void)
{
    return g_ctx.connected;
}

OPERATE_RET tcp_client_send(const char *data, uint32_t len)
{
    if (!g_ctx.connected || g_ctx.socket_fd < 0) {
        PR_WARN("Cannot send - not connected");
        return OPRT_SOCK_ERR;
    }
    
    tal_mutex_lock(g_ctx.mutex);
    
    /* Send header (length, little-endian) */
    uint8_t header[4];
    header[0] = len & 0xFF;
    header[1] = (len >> 8) & 0xFF;
    header[2] = (len >> 16) & 0xFF;
    header[3] = (len >> 24) & 0xFF;
    
    int sent = tal_net_send(g_ctx.socket_fd, header, 4);
    if (sent != 4) {
        PR_ERR("Failed to send header");
        tal_mutex_unlock(g_ctx.mutex);
        return OPRT_SOCK_ERR;
    }
    
    /* Send data */
    sent = tal_net_send(g_ctx.socket_fd, (void *)data, len);
    if (sent != (int)len) {
        PR_ERR("Failed to send data");
        tal_mutex_unlock(g_ctx.mutex);
        return OPRT_SOCK_ERR;
    }
    
    tal_mutex_unlock(g_ctx.mutex);
    
    PR_DEBUG("Sent to server: %.*s", len, data);
    return OPRT_OK;
}

OPERATE_RET tcp_client_send_str(const char *str)
{
    if (!str) return OPRT_INVALID_PARM;
    return tcp_client_send(str, strlen(str));
}

/**
 * @brief Receive raw data from TCP connection (for voice messages)
 * @param buf Buffer to store received data
 * @param len Expected number of bytes to receive
 * @param timeout_ms Maximum time to wait for data
 * @return Number of bytes actually received, or negative on error
 */
int tcp_client_receive_data(uint8_t *buf, int len, int timeout_ms)
{
    if (!g_ctx.connected || g_ctx.socket_fd < 0) {
        PR_WARN("[TCP] Cannot receive - not connected");
        return -1;
    }
    
    if (!buf || len <= 0) {
        return -1;
    }
    
    tal_mutex_lock(g_ctx.mutex);
    
    int total_received = 0;
    int start_time = tal_system_get_millisecond();
    
    /* Set longer timeout for voice data reception */
    tal_net_set_timeout(g_ctx.socket_fd, timeout_ms, TRANS_RECV);
    
    while (total_received < len) {
        /* Check timeout */
        int elapsed = tal_system_get_millisecond() - start_time;
        if (elapsed > timeout_ms) {
            PR_WARN("[TCP] Voice receive timeout after %d ms", elapsed);
            break;
        }
        
        /* Receive remaining bytes */
        int remaining = len - total_received;
        int chunk_size = remaining > 4096 ? 4096 : remaining;
        
        int recv_len = tal_net_recv(g_ctx.socket_fd, buf + total_received, chunk_size);
        
        if (recv_len > 0) {
            total_received += recv_len;
            if (total_received % 10000 < recv_len) {
                PR_DEBUG("[TCP] Voice receive progress: %d/%d bytes", total_received, len);
            }
        } else if (recv_len < 0) {
            int err = tal_net_get_errno();
            if (err == UNW_ETIMEDOUT || err == UNW_EAGAIN || err == UNW_EWOULDBLOCK) {
                /* Timeout, check overall timeout and continue */
                continue;
            }
            PR_ERR("[TCP] Voice receive error: %d", err);
            break;
        } else {
            /* recv_len == 0, connection closed? */
            PR_WARN("[TCP] Voice receive: connection closed?");
            break;
        }
    }
    
    /* Restore normal timeout */
    tal_net_set_timeout(g_ctx.socket_fd, TCP_RECV_TIMEOUT_MS, TRANS_RECV);
    
    tal_mutex_unlock(g_ctx.mutex);
    
    PR_INFO("[TCP] Received %d/%d bytes of voice data", total_received, len);
    return total_received;
}
