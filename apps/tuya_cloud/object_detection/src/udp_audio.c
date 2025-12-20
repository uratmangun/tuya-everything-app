/**
 * @file udp_audio.c
 * @brief UDP Audio streaming with G.711 u-law encoding
 * 
 * Sends audio encoded as G.711 u-law over UDP for low-latency streaming.
 * 
 * Packet format: [SEQ:1byte][G711_DATA:Nbytes]
 * 
 * Benefits of G.711:
 * - 50% bandwidth reduction (8-bit vs 16-bit)
 * - 1 byte = 1 sample (no byte alignment issues if packet dropped)
 * - Sequence numbers detect out-of-order/missing packets
 */

#include "udp_audio.h"
#include "g711_codec.h"
#include "tal_api.h"
#include "tal_network.h"
#include <string.h>

/***********************************************************
***********************macro define************************
***********************************************************/
/* Max packet size: 1 byte seq + 320 bytes G.711 (20ms at 16kHz) */
#define UDP_PACKET_MAX_SIZE 512

/***********************************************************
***********************typedef define***********************
***********************************************************/
typedef struct {
    int socket_fd;
    TUYA_IP_ADDR_T server_addr;
    uint16_t server_port;
    bool ready;
    uint8_t seq;  /* Sequence number (0-255, wraps around) */
    uint32_t packets_sent;
} udp_audio_ctx_t;

/***********************************************************
***********************variable define**********************
***********************************************************/
static udp_audio_ctx_t g_udp = {0};

/* Send buffer for G.711 encoded packets */
static uint8_t g_send_buf[UDP_PACKET_MAX_SIZE];

/***********************************************************
***********************function define**********************
***********************************************************/

OPERATE_RET udp_audio_init(const char *host, uint16_t port)
{
    if (g_udp.ready) {
        PR_WARN("UDP audio already initialized");
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
    g_udp.seq = 0;
    g_udp.packets_sent = 0;
    g_udp.ready = true;
    
    PR_NOTICE("UDP audio initialized with G.711 encoding: %s:%d", host, port);
    return OPRT_OK;
}

OPERATE_RET udp_audio_send_pcm(const int16_t *pcm_data, uint32_t pcm_samples)
{
    if (!g_udp.ready || g_udp.socket_fd < 0) {
        return OPRT_SOCK_ERR;
    }
    
    if (pcm_samples == 0 || pcm_samples > (UDP_PACKET_MAX_SIZE - 1)) {
        PR_ERR("Invalid PCM sample count: %u", pcm_samples);
        return OPRT_INVALID_PARM;
    }
    
    /* Build packet: [SEQ:1][G711_DATA:N] */
    g_send_buf[0] = g_udp.seq;
    
    /* Encode PCM to G.711 u-law */
    size_t g711_len = g711_encode_ulaw(pcm_data, pcm_samples, &g_send_buf[1]);
    
    /* Total packet size: 1 byte seq + G.711 data */
    uint32_t packet_len = 1 + g711_len;
    
    int sent = tal_net_send_to(g_udp.socket_fd, (void *)g_send_buf, packet_len, 
                                g_udp.server_addr, g_udp.server_port);
    
    if (sent != (int)packet_len) {
        PR_DEBUG("UDP send incomplete: %d/%u", sent, packet_len);
        return OPRT_SOCK_ERR;
    }
    
    /* Increment sequence number (wraps at 255) */
    g_udp.seq++;
    g_udp.packets_sent++;
    
    /* Log stats every 500 packets (~5 seconds at 100 packets/sec) */
    if (g_udp.packets_sent % 500 == 0) {
        PR_INFO("UDP audio: %u packets sent, seq=%u, last_size=%u", 
                g_udp.packets_sent, g_udp.seq, packet_len);
    }
    
    return OPRT_OK;
}

OPERATE_RET udp_audio_send(const uint8_t *data, uint32_t len)
{
    if (!g_udp.ready || g_udp.socket_fd < 0) {
        return OPRT_SOCK_ERR;
    }
    
    /* Build packet with sequence number: [SEQ:1][DATA:N] */
    if (len > (UDP_PACKET_MAX_SIZE - 1)) {
        len = UDP_PACKET_MAX_SIZE - 1;
    }
    
    g_send_buf[0] = g_udp.seq;
    memcpy(&g_send_buf[1], data, len);
    
    uint32_t packet_len = 1 + len;
    
    int sent = tal_net_send_to(g_udp.socket_fd, (void *)g_send_buf, packet_len, 
                                g_udp.server_addr, g_udp.server_port);
    
    if (sent != (int)packet_len) {
        PR_DEBUG("UDP send incomplete: %d/%u", sent, packet_len);
        return OPRT_SOCK_ERR;
    }
    
    g_udp.seq++;
    g_udp.packets_sent++;
    
    return OPRT_OK;
}

bool udp_audio_is_ready(void)
{
    return g_udp.ready;
}

uint8_t udp_audio_get_seq(void)
{
    return g_udp.seq;
}

void udp_audio_close(void)
{
    if (g_udp.socket_fd >= 0) {
        tal_net_close(g_udp.socket_fd);
        g_udp.socket_fd = -1;
    }
    g_udp.ready = false;
    g_udp.seq = 0;
    g_udp.packets_sent = 0;
    PR_NOTICE("UDP audio closed");
}

OPERATE_RET udp_audio_send_ping(void)
{
    if (!g_udp.ready || g_udp.socket_fd < 0) {
        return OPRT_SOCK_ERR;
    }
    
    /* Send minimal 1-byte ping packet (0xFF = ping marker) */
    uint8_t ping_pkt = 0xFF;
    
    int sent = tal_net_send_to(g_udp.socket_fd, (void *)&ping_pkt, 1,
                               g_udp.server_addr, g_udp.server_port);
    
    if (sent != 1) {
        PR_DEBUG("UDP ping failed: %d", sent);
        return OPRT_SOCK_ERR;
    }
    
    PR_DEBUG("UDP keepalive ping sent to %s:%d", 
             tal_net_addr2str(g_udp.server_addr), g_udp.server_port);
    return OPRT_OK;
}

