/**
 * @file speaker_streaming.c
 * @brief UDP Speaker Streaming Module for T5AI DevKit
 *
 * Receives PCM audio data via UDP from the web server and plays it
 * through the DevKit speaker. This enables two-way audio communication
 * where the browser user can talk to the DevKit.
 *
 * NAT Traversal:
 * - DevKit sends periodic UDP "ping" packets to VPS port 5002
 * - This creates a NAT mapping allowing VPS to send audio back
 * - VPS tracks DevKit's NAT-mapped address from these pings
 *
 * Audio Format:
 * - Sample Rate: 16kHz
 * - Channels: Mono
 * - Bit Depth: 16-bit signed
 * - Transport: UDP packets (raw PCM, no header)
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#include "tal_api.h"
#include "tal_network.h"
#include "tdl_audio_manage.h"
#include "tuya_config.h"
#include "ai_audio_player.h"
#include "ai_audio.h"

/* UDP port for speaker audio (same on both DevKit and VPS) */
#define SPEAKER_UDP_PORT    5002

/* PCM buffer size (max UDP payload) */
#define PCM_BUF_SIZE        1400

/* Receive timeout in milliseconds */
#define RECV_TIMEOUT_MS     100

/* NAT keepalive interval in milliseconds (send ping every 5 seconds) */
#define NAT_KEEPALIVE_MS    5000

/* Ping packet marker (0xFE = speaker ping, different from mic ping 0xFF) */
#define SPEAKER_PING_MARKER 0xFE

/* Global state */
static bool g_speaker_active = false;
static int g_udp_socket = -1;
static TDL_AUDIO_HANDLE_T g_audio_hdl = NULL;
static THREAD_HANDLE g_speaker_thread = NULL;
static THREAD_HANDLE g_keepalive_thread = NULL;

/* VPS server address for NAT hole punching */
static TUYA_IP_ADDR_T g_vps_addr = 0;
static uint16_t g_vps_port = SPEAKER_UDP_PORT;

/* Statistics */
static uint32_t g_packets_received = 0;
static uint32_t g_bytes_received = 0;
static uint32_t g_play_errors = 0;
static uint32_t g_pings_sent = 0;

/* VPS host from compile-time definition */
#ifndef TCP_SERVER_HOST
#define TCP_SERVER_HOST "13.212.218.43"  /* Default fallback */
#endif

/**
 * @brief NAT keepalive task - sends periodic pings to VPS to keep NAT hole open
 */
static void speaker_keepalive_task(void *arg)
{
    uint8_t ping_pkt = SPEAKER_PING_MARKER;
    
    PR_INFO("[SPEAKER] NAT keepalive task started -> %s:%d", 
            tal_net_addr2str(g_vps_addr), g_vps_port);
    
    while (g_speaker_active) {
        /* Send ping to VPS to punch/maintain NAT hole */
        int sent = tal_net_send_to(g_udp_socket, &ping_pkt, 1, g_vps_addr, g_vps_port);
        if (sent == 1) {
            g_pings_sent++;
            if (g_pings_sent % 12 == 1) {  /* Log every minute */
                PR_DEBUG("[SPEAKER] NAT keepalive ping #%u sent", g_pings_sent);
            }
        } else {
            PR_WARN("[SPEAKER] NAT keepalive ping failed: %d", sent);
        }
        
        /* Wait before next ping */
        tal_system_sleep(NAT_KEEPALIVE_MS);
    }
    
    PR_INFO("[SPEAKER] NAT keepalive task stopped (sent %u pings)", g_pings_sent);
}

/**
 * @brief Speaker RX task - receives UDP audio and plays it
 */
static void speaker_rx_task(void *arg)
{
    uint8_t buf[PCM_BUF_SIZE];
    TUYA_IP_ADDR_T addr = 0;
    uint16_t port = 0;
    TUYA_ERRNO len;

    PR_INFO("[SPEAKER] UDP receive task started on port %d", SPEAKER_UDP_PORT);

    /* Find audio handle */
    OPERATE_RET rt = tdl_audio_find(AUDIO_CODEC_NAME, &g_audio_hdl);
    if (rt != OPRT_OK || g_audio_hdl == NULL) {
        PR_ERR("[SPEAKER] Failed to find audio codec: %d", rt);
        return;
    }
    PR_INFO("[SPEAKER] Audio codec found: %s", AUDIO_CODEC_NAME);

    while (g_speaker_active) {
        /* Receive UDP packet (may timeout and return <= 0) */
        len = tal_net_recvfrom(g_udp_socket, buf, sizeof(buf), &addr, &port);
        
        if (len <= 0) {
            /* Timeout or error - continue waiting */
            continue;
        }
        
        /* Ignore ping responses (1 byte packets) */
        if (len == 1) {
            continue;
        }

        /* Got audio data - play it */
        g_packets_received++;
        g_bytes_received += len;

        if (g_packets_received % 100 == 1) {
            PR_DEBUG("[SPEAKER] Packet #%u: %d bytes from %s:%d", 
                     g_packets_received, (int)len, 
                     tal_net_addr2str(addr), port);
        }

        /* Play PCM data through speaker */
        rt = tdl_audio_play(g_audio_hdl, buf, len);
        if (rt != OPRT_OK) {
            g_play_errors++;
            if (g_play_errors % 100 == 1) {
                PR_WARN("[SPEAKER] Play error #%u: %d", g_play_errors, rt);
            }
        }
    }

    PR_INFO("[SPEAKER] UDP receive task stopped");
}

/**
 * @brief Initialize speaker streaming module
 *
 * Creates UDP socket and starts receiver thread.
 * Also starts NAT keepalive thread to punch through NAT.
 *
 * @return OPRT_OK on success
 */
OPERATE_RET speaker_streaming_init(void)
{
    OPERATE_RET rt;

    if (g_speaker_active) {
        PR_WARN("[SPEAKER] Already initialized");
        return OPRT_OK;
    }

    PR_INFO("[SPEAKER] Initializing UDP speaker streaming on port %d...", SPEAKER_UDP_PORT);

    /* Use compile-time VPS host */
    const char *vps_host = TCP_SERVER_HOST;
    PR_INFO("[SPEAKER] Using VPS host: %s", vps_host);
    
    g_vps_addr = tal_net_str2addr(vps_host);
    if (g_vps_addr == 0) {
        /* Try DNS lookup */
        rt = tal_net_gethostbyname(vps_host, &g_vps_addr);
        if (rt != OPRT_OK || g_vps_addr == 0) {
            PR_ERR("[SPEAKER] Failed to resolve VPS address: %s", vps_host);
            return OPRT_COM_ERROR;
        }
    }
    PR_INFO("[SPEAKER] VPS address resolved: %s -> %s", 
            vps_host, tal_net_addr2str(g_vps_addr));

    /* Create UDP socket */
    g_udp_socket = tal_net_socket_create(PROTOCOL_UDP);
    if (g_udp_socket < 0) {
        PR_ERR("[SPEAKER] Failed to create UDP socket");
        return OPRT_COM_ERROR;
    }

    /* Set socket receive timeout for non-blocking behavior */
    rt = tal_net_set_timeout(g_udp_socket, RECV_TIMEOUT_MS, TRANS_RECV);
    if (rt != OPRT_OK) {
        PR_WARN("[SPEAKER] Failed to set socket timeout: %d", rt);
    }

    /* Bind to local port (use 0 for auto-assign, since we connect outbound) */
    /* We bind to the same port so VPS can send back to us */
    rt = tal_net_bind(g_udp_socket, TY_IPADDR_ANY, SPEAKER_UDP_PORT);
    if (rt != OPRT_OK) {
        PR_WARN("[SPEAKER] Failed to bind to port %d, using ephemeral port", SPEAKER_UDP_PORT);
        /* Continue anyway - we'll use ephemeral port */
    }

    PR_INFO("[SPEAKER] UDP socket created");

    /* Mark as active */
    g_speaker_active = true;
    
    /* Start receiver thread */
    THREAD_CFG_T rx_cfg = {
        .stackDepth = 4096,
        .priority = THREAD_PRIO_3,
        .thrdname = "speaker_udp"
    };

    rt = tal_thread_create_and_start(&g_speaker_thread, NULL, NULL, 
                                      speaker_rx_task, NULL, &rx_cfg);
    if (rt != OPRT_OK) {
        PR_ERR("[SPEAKER] Failed to create receiver thread: %d", rt);
        g_speaker_active = false;
        tal_net_close(g_udp_socket);
        g_udp_socket = -1;
        return rt;
    }

    /* Start NAT keepalive thread */
    THREAD_CFG_T ka_cfg = {
        .stackDepth = 2048,
        .priority = THREAD_PRIO_4,
        .thrdname = "spk_keepalive"
    };

    rt = tal_thread_create_and_start(&g_keepalive_thread, NULL, NULL, 
                                      speaker_keepalive_task, NULL, &ka_cfg);
    if (rt != OPRT_OK) {
        PR_WARN("[SPEAKER] Failed to create keepalive thread: %d (NAT may not work)", rt);
        /* Continue anyway - audio might still work on some networks */
    }

    PR_NOTICE("[SPEAKER] Speaker streaming initialized successfully");
    PR_NOTICE("[SPEAKER] NAT hole punching to %s:%d every %dms", 
              tal_net_addr2str(g_vps_addr), g_vps_port, NAT_KEEPALIVE_MS);
    
    return OPRT_OK;
}

/**
 * @brief Stop speaker streaming
 */
OPERATE_RET speaker_streaming_stop(void)
{
    if (!g_speaker_active) {
        return OPRT_OK;
    }

    PR_INFO("[SPEAKER] Stopping speaker streaming...");

    g_speaker_active = false;

    /* Give threads time to exit */
    tal_system_sleep(200);

    /* Close socket */
    if (g_udp_socket >= 0) {
        tal_net_close(g_udp_socket);
        g_udp_socket = -1;
    }

    /* Delete threads */
    if (g_speaker_thread) {
        tal_thread_delete(g_speaker_thread);
        g_speaker_thread = NULL;
    }
    if (g_keepalive_thread) {
        tal_thread_delete(g_keepalive_thread);
        g_keepalive_thread = NULL;
    }

    PR_INFO("[SPEAKER] Speaker streaming stopped");
    PR_INFO("[SPEAKER] Stats: %u packets, %u bytes, %u errors, %u pings",
            g_packets_received, g_bytes_received, g_play_errors, g_pings_sent);

    return OPRT_OK;
}

/**
 * @brief Check if speaker streaming is active
 */
bool speaker_streaming_is_active(void)
{
    return g_speaker_active;
}

/**
 * @brief Get speaker streaming statistics
 */
void speaker_streaming_get_stats(uint32_t *packets, uint32_t *bytes, uint32_t *errors)
{
    if (packets) *packets = g_packets_received;
    if (bytes) *bytes = g_bytes_received;
    if (errors) *errors = g_play_errors;
}
