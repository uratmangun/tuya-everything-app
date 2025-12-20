/**
 * @file udp_audio.c
 * @brief UDP Audio streaming - sends raw PCM via UDP for low latency
 */

#include "udp_audio.h"
#include "tal_api.h"
#include "tal_network.h"
#include <string.h>

/***********************************************************
***********************typedef define***********************
***********************************************************/
typedef struct {
    int socket_fd;
    TUYA_IP_ADDR_T server_addr;
    uint16_t server_port;
    bool ready;
} udp_audio_ctx_t;

/***********************************************************
***********************variable define**********************
***********************************************************/
static udp_audio_ctx_t g_udp = {0};

/***********************************************************
***********************function define**********************
***********************************************************/

OPERATE_RET udp_audio_init(const char *host, uint16_t port)
{
    if (g_udp.ready) {
        return OPRT_OK;
    }
    
    /* Create UDP socket */
    g_udp.socket_fd = tal_net_socket_create(PROTOCOL_UDP);
    if (g_udp.socket_fd < 0) {
        PR_ERR("Failed to create UDP socket");
        return OPRT_SOCK_ERR;
    }
    
    /* Resolve server address */
    g_udp.server_addr = tal_net_str2addr(host);
    if (g_udp.server_addr == 0) {
        PR_ERR("Failed to resolve UDP host: %s", host);
        tal_net_close(g_udp.socket_fd);
        g_udp.socket_fd = -1;
        return OPRT_SOCK_ERR;
    }
    
    g_udp.server_port = port;
    g_udp.ready = true;
    
    PR_NOTICE("UDP audio initialized: %s:%d", host, port);
    return OPRT_OK;
}

OPERATE_RET udp_audio_send(const uint8_t *data, uint32_t len)
{
    if (!g_udp.ready || g_udp.socket_fd < 0) {
        return OPRT_SOCK_ERR;
    }
    
    int sent = tal_net_send_to(g_udp.socket_fd, (void *)data, len, 
                                g_udp.server_addr, g_udp.server_port);
    
    if (sent != (int)len) {
        PR_DEBUG("UDP send incomplete: %d/%u", sent, len);
        return OPRT_SOCK_ERR;
    }
    
    return OPRT_OK;
}

bool udp_audio_is_ready(void)
{
    return g_udp.ready;
}
