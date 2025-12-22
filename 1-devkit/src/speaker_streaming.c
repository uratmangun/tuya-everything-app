/**
 * @file speaker_streaming.c
 * @brief UDP Speaker Streaming Module for T5AI DevKit with Jitter Buffer
 *
 * Receives PCM audio data via UDP from the web server and plays it
 * through the DevKit speaker. This enables two-way audio communication
 * where the browser user can talk to the DevKit.
 *
 * Features:
 * - NAT Hole Punching: Periodic UDP pings to VPS maintain NAT mapping
 * - Jitter Buffer: Ring buffer absorbs network timing variations
 * - Prefill: 100ms buffer fill before playback starts
 *
 * Audio Format (Hardware Contract):
 * - Sample Rate: 16kHz
 * - Channels: Mono
 * - Bit Depth: 16-bit signed, Little Endian
 * - Packet Size: 640 bytes (20ms of audio)
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#include "tal_api.h"
#include "tal_network.h"
#include "tdl_audio_manage.h"
#include "tuya_config.h"
#include "ai_audio_player.h"
#include "ai_audio.h"
#include <string.h>

/* UDP port for speaker audio (same on both DevKit and VPS) */
#define SPEAKER_UDP_PORT    5002

/* PCM buffer size (max UDP payload) */
#define PCM_BUF_SIZE        1400

/* Receive timeout in milliseconds */
#define RECV_TIMEOUT_MS     50

/* NAT keepalive interval in milliseconds (send ping every 5 seconds) */
#define NAT_KEEPALIVE_MS    5000

/* Ping packet marker (0xFE = speaker ping, different from mic ping 0xFF) */
#define SPEAKER_PING_MARKER 0xFE

/* ========== Jitter Buffer Configuration ========== */
/* 3 seconds of 16kHz/16-bit mono audio = 96000 bytes (uses PSRAM) */
#define JITTER_BUFFER_SIZE  96000

/* Prefill threshold: 500ms = 16000 bytes before playback starts */
/* Larger prefill = more latency but much better stability */
#define PREFILL_THRESHOLD   16000

/* Playback chunk size: 20ms = 640 bytes (matches server packet size) */
#define PLAYBACK_CHUNK_SIZE 640

/* Playback interval: 20ms per chunk */
#define PLAYBACK_INTERVAL_MS 20

/* Global state */
static bool g_speaker_active = false;
static int g_udp_socket = -1;
static TDL_AUDIO_HANDLE_T g_audio_hdl = NULL;
static THREAD_HANDLE g_rx_thread = NULL;
static THREAD_HANDLE g_playback_thread = NULL;
static THREAD_HANDLE g_keepalive_thread = NULL;

/* VPS server address for NAT hole punching */
static TUYA_IP_ADDR_T g_vps_addr = 0;
static uint16_t g_vps_port = SPEAKER_UDP_PORT;

/* Jitter buffer (ring buffer) */
static uint8_t g_jitter_buffer[JITTER_BUFFER_SIZE];
static volatile uint32_t g_buf_head = 0;      /* Write position */
static volatile uint32_t g_buf_tail = 0;      /* Read position */
static volatile uint32_t g_buf_count = 0;     /* Bytes in buffer */
static volatile bool g_playback_started = false;

/* Mutex for buffer access */
static MUTEX_HANDLE g_buf_mutex = NULL;

/* Statistics */
static uint32_t g_packets_received = 0;
static uint32_t g_bytes_received = 0;
static uint32_t g_play_errors = 0;
static uint32_t g_pings_sent = 0;
static uint32_t g_underruns = 0;
static uint32_t g_overruns = 0;

/* VPS host storage for dynamic configuration */
static char g_vps_host[64] = "";

/**
 * @brief Write data to jitter buffer (producer)
 */
static void jitter_buffer_write(const uint8_t *data, uint32_t len)
{
    if (len == 0 || data == NULL) return;
    
    tal_mutex_lock(g_buf_mutex);
    
    /* Check for buffer overflow */
    if (g_buf_count + len > JITTER_BUFFER_SIZE) {
        /* Buffer overflow - drop oldest data */
        uint32_t overflow = (g_buf_count + len) - JITTER_BUFFER_SIZE;
        g_buf_tail = (g_buf_tail + overflow) % JITTER_BUFFER_SIZE;
        g_buf_count -= overflow;
        g_overruns++;
        if (g_overruns % 50 == 1) {
            PR_WARN("[SPEAKER] Buffer overrun #%u (dropped %u bytes)", g_overruns, overflow);
        }
    }
    
    /* Write data to ring buffer */
    for (uint32_t i = 0; i < len; i++) {
        g_jitter_buffer[g_buf_head] = data[i];
        g_buf_head = (g_buf_head + 1) % JITTER_BUFFER_SIZE;
    }
    g_buf_count += len;
    
    tal_mutex_unlock(g_buf_mutex);
}

/**
 * @brief Read data from jitter buffer (consumer)
 * @return Number of bytes read (may be less than requested if buffer underrun)
 */
static uint32_t jitter_buffer_read(uint8_t *data, uint32_t len)
{
    tal_mutex_lock(g_buf_mutex);
    
    uint32_t available = g_buf_count;
    uint32_t to_read = (len < available) ? len : available;
    
    /* Read data from ring buffer */
    for (uint32_t i = 0; i < to_read; i++) {
        data[i] = g_jitter_buffer[g_buf_tail];
        g_buf_tail = (g_buf_tail + 1) % JITTER_BUFFER_SIZE;
    }
    g_buf_count -= to_read;
    
    tal_mutex_unlock(g_buf_mutex);
    
    return to_read;
}

/**
 * @brief Get current buffer level
 */
static uint32_t jitter_buffer_level(void)
{
    tal_mutex_lock(g_buf_mutex);
    uint32_t count = g_buf_count;
    tal_mutex_unlock(g_buf_mutex);
    return count;
}

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
                PR_DEBUG("[SPEAKER] NAT ping #%u, buffer: %u bytes", g_pings_sent, jitter_buffer_level());
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
 * @brief UDP receiver task - writes incoming audio to jitter buffer
 */
static void speaker_rx_task(void *arg)
{
    uint8_t buf[PCM_BUF_SIZE];
    TUYA_IP_ADDR_T addr = 0;
    uint16_t port = 0;
    TUYA_ERRNO len;

    PR_INFO("[SPEAKER] UDP receiver task started on port %d", SPEAKER_UDP_PORT);

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

        /* Got audio data - write to jitter buffer */
        g_packets_received++;
        g_bytes_received += len;

        if (g_packets_received % 200 == 1) {
            PR_DEBUG("[SPEAKER] RX packet #%u: %d bytes, buffer: %u bytes", 
                     g_packets_received, (int)len, jitter_buffer_level());
        }

        /* Write to jitter buffer */
        jitter_buffer_write(buf, len);
    }

    PR_INFO("[SPEAKER] UDP receiver task stopped");
}

/**
 * @brief Playback task - reads from jitter buffer and plays audio at steady rate
 */
static void speaker_playback_task(void *arg)
{
    uint8_t chunk[PLAYBACK_CHUNK_SIZE];
    OPERATE_RET rt;
    uint32_t chunks_played = 0;
    
    PR_INFO("[SPEAKER] Playback task started");
    PR_INFO("[SPEAKER] Waiting for buffer prefill (%u bytes = %ums)...", 
            PREFILL_THRESHOLD, PREFILL_THRESHOLD / 32);

    /* Find audio handle */
    rt = tdl_audio_find(AUDIO_CODEC_NAME, &g_audio_hdl);
    if (rt != OPRT_OK || g_audio_hdl == NULL) {
        PR_ERR("[SPEAKER] Failed to find audio codec: %d", rt);
        return;
    }
    PR_INFO("[SPEAKER] Audio codec found: %s", AUDIO_CODEC_NAME);

    /* Wait for prefill threshold */
    while (g_speaker_active && !g_playback_started) {
        uint32_t level = jitter_buffer_level();
        if (level >= PREFILL_THRESHOLD) {
            g_playback_started = true;
            PR_NOTICE("[SPEAKER] Buffer prefilled (%u bytes) - starting playback!", level);
        } else {
            tal_system_sleep(10);
        }
    }

    /* Main playback loop */
    while (g_speaker_active) {
        uint32_t level = jitter_buffer_level();
        
        if (level >= PLAYBACK_CHUNK_SIZE) {
            /* Read chunk from jitter buffer */
            uint32_t read = jitter_buffer_read(chunk, PLAYBACK_CHUNK_SIZE);
            
            if (read == PLAYBACK_CHUNK_SIZE) {
                /* Play PCM data through speaker */
                /* tdl_audio_play() should block until audio buffer has space */
                rt = tdl_audio_play(g_audio_hdl, chunk, PLAYBACK_CHUNK_SIZE);
                if (rt != OPRT_OK) {
                    g_play_errors++;
                    if (g_play_errors % 100 == 1) {
                        PR_WARN("[SPEAKER] Play error #%u: %d", g_play_errors, rt);
                    }
                } else {
                    chunks_played++;
                    if (chunks_played % 500 == 0) {
                        PR_DEBUG("[SPEAKER] Played %u chunks, buffer: %u bytes", 
                                 chunks_played, jitter_buffer_level());
                    }
                }
                /* No sleep needed - tdl_audio_play should pace itself */
            }
        } else {
            /* Buffer underrun - wait for more data instead of playing silence */
            g_underruns++;
            if (g_underruns % 100 == 1) {
                PR_WARN("[SPEAKER] Buffer underrun #%u (only %u bytes available)", 
                        g_underruns, level);
            }
            
            /* Wait for more data to arrive */
            tal_system_sleep(PLAYBACK_INTERVAL_MS);
        }
        /* No sleep here - let audio driver pace the playback */
    }

    PR_INFO("[SPEAKER] Playback task stopped (played %u chunks)", chunks_played);
}

/**
 * @brief Initialize speaker streaming module
 * @param host VPS server host for NAT hole punching (same as TCP server)
 */
OPERATE_RET speaker_streaming_init(const char *host)
{
    OPERATE_RET rt;

    if (!host || strlen(host) == 0) {
        PR_ERR("[SPEAKER] Invalid host parameter");
        return OPRT_INVALID_PARM;
    }

    if (g_speaker_active) {
        PR_WARN("[SPEAKER] Already initialized");
        return OPRT_OK;
    }

    /* Store the VPS host for later use */
    strncpy(g_vps_host, host, sizeof(g_vps_host) - 1);
    g_vps_host[sizeof(g_vps_host) - 1] = '\0';

    PR_INFO("[SPEAKER] Initializing with jitter buffer (%u bytes, prefill %u bytes)...",
            JITTER_BUFFER_SIZE, PREFILL_THRESHOLD);

    /* Create buffer mutex */
    rt = tal_mutex_create_init(&g_buf_mutex);
    if (rt != OPRT_OK) {
        PR_ERR("[SPEAKER] Failed to create mutex: %d", rt);
        return rt;
    }

    /* Reset buffer state */
    g_buf_head = 0;
    g_buf_tail = 0;
    g_buf_count = 0;
    g_playback_started = false;
    memset(g_jitter_buffer, 0, JITTER_BUFFER_SIZE);

    /* Use the host provided at init */
    PR_INFO("[SPEAKER] Using VPS host: %s", g_vps_host);
    
    g_vps_addr = tal_net_str2addr(g_vps_host);
    if (g_vps_addr == 0) {
        rt = tal_net_gethostbyname(g_vps_host, &g_vps_addr);
        if (rt != OPRT_OK || g_vps_addr == 0) {
            PR_ERR("[SPEAKER] Failed to resolve VPS address: %s", g_vps_host);
            tal_mutex_release(g_buf_mutex);
            g_buf_mutex = NULL;
            return OPRT_COM_ERROR;
        }
    }
    PR_INFO("[SPEAKER] VPS address: %s -> %s", g_vps_host, tal_net_addr2str(g_vps_addr));

    /* Create UDP socket */
    g_udp_socket = tal_net_socket_create(PROTOCOL_UDP);
    if (g_udp_socket < 0) {
        PR_ERR("[SPEAKER] Failed to create UDP socket");
        tal_mutex_release(g_buf_mutex);
        g_buf_mutex = NULL;
        return OPRT_COM_ERROR;
    }

    /* Set socket receive timeout */
    tal_net_set_timeout(g_udp_socket, RECV_TIMEOUT_MS, TRANS_RECV);

    /* Bind to local port */
    rt = tal_net_bind(g_udp_socket, TY_IPADDR_ANY, SPEAKER_UDP_PORT);
    if (rt != OPRT_OK) {
        PR_WARN("[SPEAKER] Failed to bind to port %d", SPEAKER_UDP_PORT);
    }

    /* Mark as active */
    g_speaker_active = true;
    
    /* Start receiver thread */
    THREAD_CFG_T rx_cfg = {
        .stackDepth = 4096,
        .priority = THREAD_PRIO_2,  /* Higher priority for receiving */
        .thrdname = "spk_rx"
    };
    rt = tal_thread_create_and_start(&g_rx_thread, NULL, NULL, 
                                      speaker_rx_task, NULL, &rx_cfg);
    if (rt != OPRT_OK) {
        PR_ERR("[SPEAKER] Failed to create receiver thread: %d", rt);
        goto cleanup;
    }

    /* Start playback thread */
    THREAD_CFG_T play_cfg = {
        .stackDepth = 4096,
        .priority = THREAD_PRIO_3,  /* Medium priority for playback */
        .thrdname = "spk_play"
    };
    rt = tal_thread_create_and_start(&g_playback_thread, NULL, NULL, 
                                      speaker_playback_task, NULL, &play_cfg);
    if (rt != OPRT_OK) {
        PR_ERR("[SPEAKER] Failed to create playback thread: %d", rt);
        goto cleanup;
    }

    /* Start NAT keepalive thread */
    THREAD_CFG_T ka_cfg = {
        .stackDepth = 2048,
        .priority = THREAD_PRIO_4,  /* Lower priority for keepalive */
        .thrdname = "spk_nat"
    };
    rt = tal_thread_create_and_start(&g_keepalive_thread, NULL, NULL, 
                                      speaker_keepalive_task, NULL, &ka_cfg);
    if (rt != OPRT_OK) {
        PR_WARN("[SPEAKER] Failed to create keepalive thread: %d", rt);
    }

    PR_NOTICE("[SPEAKER] Speaker streaming initialized!");
    PR_NOTICE("[SPEAKER] Config: %ukHz/16bit/mono, %ums chunks, %ums prefill", 
              16, PLAYBACK_INTERVAL_MS, PREFILL_THRESHOLD / 32);
    
    return OPRT_OK;

cleanup:
    g_speaker_active = false;
    if (g_udp_socket >= 0) {
        tal_net_close(g_udp_socket);
        g_udp_socket = -1;
    }
    if (g_buf_mutex) {
        tal_mutex_release(g_buf_mutex);
        g_buf_mutex = NULL;
    }
    return rt;
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
    tal_system_sleep(PLAYBACK_INTERVAL_MS * 3);

    /* Close socket */
    if (g_udp_socket >= 0) {
        tal_net_close(g_udp_socket);
        g_udp_socket = -1;
    }

    /* Delete threads */
    if (g_rx_thread) {
        tal_thread_delete(g_rx_thread);
        g_rx_thread = NULL;
    }
    if (g_playback_thread) {
        tal_thread_delete(g_playback_thread);
        g_playback_thread = NULL;
    }
    if (g_keepalive_thread) {
        tal_thread_delete(g_keepalive_thread);
        g_keepalive_thread = NULL;
    }

    /* Release mutex */
    if (g_buf_mutex) {
        tal_mutex_release(g_buf_mutex);
        g_buf_mutex = NULL;
    }

    PR_INFO("[SPEAKER] Stopped. Stats: RX %u pkts/%u bytes, underruns %u, overruns %u, errors %u",
            g_packets_received, g_bytes_received, g_underruns, g_overruns, g_play_errors);

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
