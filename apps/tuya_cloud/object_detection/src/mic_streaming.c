/**
 * @file mic_streaming.c
 * @brief Microphone audio streaming over UDP for WebRTC/Opus encoding
 * 
 * Captures audio from the onboard microphone, sends raw PCM via UDP
 * to the VPS for server-side Opus encoding and WebRTC streaming.
 * 
 * WebRTC/Opus benefits:
 * - WebRTC jitter buffer handles packet loss
 * - Opus provides excellent compression (~24kbps)
 * - Native browser playback (no custom decoder)
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#include "mic_streaming.h"
#include "udp_audio.h"
#include "tal_api.h"
#include "tdl_audio_manage.h"
#include "tuya_ringbuf.h"
#include <string.h>

/***********************************************************
************************macro define************************
***********************************************************/
/* Audio configuration - T5AI mic runs at 16kHz 16-bit */
#define MIC_SAMPLE_RATE     16000
#define MIC_BITS            16
#define MIC_CHANNELS        1
#define MIC_FRAME_MS        20       /* 20ms frames */

/* Frame size in samples (16kHz * 20ms = 320 samples) */
#define MIC_FRAME_SAMPLES   320

/* Frame size in bytes for PCM (320 samples * 2 bytes = 640 bytes) */
#define MIC_FRAME_SIZE_PCM  (MIC_FRAME_SAMPLES * 2)

/* Ring buffer for audio data - 2 seconds buffer */
#define MIC_RINGBUF_SIZE    (MIC_SAMPLE_RATE * 2 * 2)  /* 16kHz * 2 bytes * 2 sec = 64KB */

/* Streaming task configuration */
#define MIC_STREAM_TASK_STACK   4096
#define MIC_STREAM_TASK_PRIO    THREAD_PRIO_2
#define MIC_STREAM_INTERVAL_MS  10   /* Check every 10ms */

/* Audio watchdog configuration - restarts driver if silent for too long */
#define MIC_WATCHDOG_TIMEOUT_MS 5000  /* 5 seconds without mic data triggers restart */
#define MIC_WATCHDOG_EMPTY_COUNT (MIC_WATCHDOG_TIMEOUT_MS / MIC_STREAM_INTERVAL_MS)

/* UDP keepalive configuration */
#define UDP_KEEPALIVE_INTERVAL_SEC  25   /* Send ping every 25 seconds */
#define UDP_KEEPALIVE_TASK_STACK    2048

/* UDP port for audio streaming */
#define UDP_AUDIO_PORT      5001

/***********************************************************
***********************typedef define***********************
***********************************************************/
typedef struct {
    bool initialized;
    bool streaming;
    TDL_AUDIO_HANDLE_T audio_hdl;
    TUYA_RINGBUFF_T ringbuf;
    THREAD_HANDLE stream_thread;
    THREAD_HANDLE keepalive_thread;  /* UDP keepalive thread */
    uint32_t total_bytes_captured;
    uint32_t total_frames_sent;
    uint32_t dropped_frames;
    uint32_t last_send_time;  /* Last time audio was sent (for keepalive logic) */
    uint32_t watchdog_restarts;  /* Count of automatic driver restarts */
} mic_streaming_ctx_t;

/***********************************************************
***********************variable define**********************
***********************************************************/
static mic_streaming_ctx_t g_mic_ctx = {0};


/***********************************************************
***********************function define**********************
***********************************************************/

/**
 * @brief Audio frame callback from audio driver
 * 
 * Called by the audio driver when a new frame is available.
 * This runs in the audio driver's context, so we just copy
 * to the ring buffer.
 */
static void mic_audio_frame_callback(TDL_AUDIO_FRAME_FORMAT_E type, 
                                      TDL_AUDIO_STATUS_E status,
                                      uint8_t *data, uint32_t len)
{
    static uint32_t callback_count = 0;
    callback_count++;
    
    /* Log every 100 callbacks to confirm mic is working */
    if (callback_count % 100 == 1) {
        PR_DEBUG("Mic callback #%u: streaming=%d, type=%d, len=%u", 
                 callback_count, g_mic_ctx.streaming, type, len);
    }
    
    if (!g_mic_ctx.streaming || !g_mic_ctx.ringbuf) {
        return;
    }
    
    /* Only handle PCM data */
    if (type != TDL_AUDIO_FRAME_FORMAT_PCM) {
        return;
    }
    
    g_mic_ctx.total_bytes_captured += len;
    
    /* Write to ring buffer (will drop oldest data if full with stop type) */
    uint32_t written = tuya_ring_buff_write(g_mic_ctx.ringbuf, data, len);
    if (written < len) {
        g_mic_ctx.dropped_frames++;
        if (g_mic_ctx.dropped_frames % 50 == 1) {
            uint32_t used = tuya_ring_buff_used_size_get(g_mic_ctx.ringbuf);
            PR_WARN("Ring buffer full! Dropped %u bytes (total drops: %u, used: %u)", 
                    len - written, g_mic_ctx.dropped_frames, used);
        }
    }
}

/**
 * @brief UDP keepalive task - keeps NAT port mapping alive
 * 
 * When audio is NOT being streamed, sends a ping every 25 seconds
 * to prevent router NAT timeout (typically 30-60 seconds for UDP).
 */
static void udp_keepalive_task(void *arg)
{
    PR_INFO("UDP keepalive task started");
    
    while (udp_audio_is_ready()) {
        /* Only send ping if we haven't sent audio recently */
        uint32_t now = tal_system_get_millisecond();
        uint32_t time_since_last_send = now - g_mic_ctx.last_send_time;
        
        /* If no audio sent in last 20 seconds, send a keepalive ping */
        if (time_since_last_send > (20 * 1000)) {
            OPERATE_RET rt = udp_audio_send_ping();
            if (rt == OPRT_OK) {
                PR_DEBUG("UDP keepalive ping sent (no audio for %u ms)", time_since_last_send);
            }
        }
        
        /* Wait 25 seconds before next check */
        for (int i = 0; i < UDP_KEEPALIVE_INTERVAL_SEC && udp_audio_is_ready(); i++) {
            tal_system_sleep(1000);
        }
    }
    
    PR_INFO("UDP keepalive task exiting");
}

/**
 * @brief Streaming task - reads PCM from ringbuf, sends via UDP for server-side encoding
 */
static void mic_streaming_task(void *arg)
{
    /* Buffer for PCM data (in bytes) */
    uint8_t pcm_buffer[MIC_FRAME_SIZE_PCM];
    uint32_t data_len;
    uint32_t send_count = 0;
    uint32_t loop_count = 0;
    uint32_t empty_count = 0;
    uint32_t last_heartbeat = 0;
    uint32_t drop_count = 0;
    
    /* Max buffer threshold: 200ms of audio = 10 frames * 640 bytes = 6400 bytes */
    const uint32_t MAX_BUFFER_BYTES = 6400;
    
    PR_INFO("Mic streaming task started (raw PCM for WebRTC/Opus)");
    
    while (g_mic_ctx.streaming) {
        loop_count++;
        
        /* Heartbeat every 5 seconds to show thread is alive */
        if ((loop_count - last_heartbeat) >= 500) {  /* 500 * 10ms = 5 seconds */
            PR_INFO("Mic stream heartbeat: loops=%u, sends=%u, empty=%u, drops=%u, captured=%u bytes, restarts=%u", 
                     loop_count, send_count, empty_count, drop_count, 
                     g_mic_ctx.total_bytes_captured, g_mic_ctx.watchdog_restarts);
            last_heartbeat = loop_count;
        }
        
        /* Check if ring buffer is valid */
        if (!g_mic_ctx.ringbuf) {
            PR_ERR("Ring buffer is NULL! Exiting streaming task.");
            break;
        }
        
        /* Check how much data is available */
        data_len = tuya_ring_buff_used_size_get(g_mic_ctx.ringbuf);
        
        /* LATENCY PROTECTION: If buffer has more than 200ms of audio, drop old data */
        if (data_len > MAX_BUFFER_BYTES) {
            uint32_t to_drop = data_len - MIC_FRAME_SIZE_PCM;  /* Keep only 1 frame */
            PR_WARN("Buffer bloat! Dropping %u bytes to catch up to real-time", to_drop);
            
            /* Discard old audio by reading and throwing away */
            while (tuya_ring_buff_used_size_get(g_mic_ctx.ringbuf) > MIC_FRAME_SIZE_PCM) {
                tuya_ring_buff_read(g_mic_ctx.ringbuf, pcm_buffer, MIC_FRAME_SIZE_PCM);
                drop_count++;
            }
            data_len = tuya_ring_buff_used_size_get(g_mic_ctx.ringbuf);
        }
        
        /* Process frames while we have enough data */
        while (data_len >= MIC_FRAME_SIZE_PCM && udp_audio_is_ready()) {
            empty_count = 0;  /* Reset empty counter */
            
            /* Read one frame of PCM data */
            uint32_t read_len = tuya_ring_buff_read(g_mic_ctx.ringbuf, 
                                                     pcm_buffer, 
                                                     MIC_FRAME_SIZE_PCM);
            
            if (read_len == MIC_FRAME_SIZE_PCM) {
                /* Send raw PCM via UDP (server encodes to Opus) */
                /* pcm_buffer contains PCM bytes, convert to samples count */
                int16_t *pcm_samples = (int16_t *)pcm_buffer;
                uint32_t num_samples = read_len / 2;  /* 2 bytes per sample */
                
                OPERATE_RET rt = udp_audio_send_pcm(pcm_samples, num_samples);
                if (rt == OPRT_OK) {
                    g_mic_ctx.total_frames_sent++;
                    g_mic_ctx.last_send_time = tal_system_get_millisecond();
                    send_count++;
                    
                    /* Log every 100 sends (~2 seconds at 50 frames/sec) */
                    if (send_count % 100 == 0) {
                        PR_INFO("Mic PCM sent: %u frames, seq=%u", 
                                g_mic_ctx.total_frames_sent, udp_audio_get_seq());
                    }
                } else {
                    PR_WARN("UDP PCM send failed: %d", rt);
                    break;  /* Stop trying if UDP fails */
                }
            } else {
                PR_WARN("Ring buffer read returned %u instead of %u", read_len, MIC_FRAME_SIZE_PCM);
                break;
            }
            
            /* Update data_len for next iteration */
            data_len = tuya_ring_buff_used_size_get(g_mic_ctx.ringbuf);
        }
        
        if (data_len < MIC_FRAME_SIZE_PCM) {
            empty_count++;
            
            /* AUDIO WATCHDOG: Detect driver stall and warn */
            if (empty_count == MIC_WATCHDOG_EMPTY_COUNT) {
                g_mic_ctx.watchdog_restarts++;
                
                PR_ERR("Audio Watchdog: Mic driver stalled (no data for %d ms)!",
                        MIC_WATCHDOG_TIMEOUT_MS);
                PR_ERR("  Streaming will continue with silence. Use 'mic off' then 'mic on' to restart.");
                PR_ERR("  Stall count: %u", g_mic_ctx.watchdog_restarts);
                
                /* 
                 * NOTE: We cannot restart the T5AI audio driver here because:
                 * - The BK7258 audio ADC keeps internal state ("aud adc is init already")
                 * - tdl_audio_close() + tdl_audio_open() causes hardware conflicts
                 * - The underlying onboard_mic_stream requires full pipeline restart
                 * 
                 * The recommended fix is to use 'mic off' then 'mic on' from the web UI
                 * which properly stops/starts the mic streaming system.
                 */
            }
            
            /* Continue logging every 30 seconds after initial stall */
            if (empty_count > MIC_WATCHDOG_EMPTY_COUNT && 
                (empty_count % (30000 / MIC_STREAM_INTERVAL_MS)) == 0) {
                PR_WARN("Audio Watchdog: Still no mic data (%u ms since last data)",
                        empty_count * MIC_STREAM_INTERVAL_MS);
            }
        }
        
        /* Wait before next check */
        tal_system_sleep(MIC_STREAM_INTERVAL_MS);
    }
    
    PR_NOTICE("Mic streaming task EXITING! streaming=%d, loop_count=%u", 
              g_mic_ctx.streaming, loop_count);
}

OPERATE_RET mic_streaming_init(void)
{
    OPERATE_RET rt = OPRT_OK;
    
    if (g_mic_ctx.initialized) {
        PR_WARN("Mic streaming already initialized");
        return OPRT_OK;
    }
    
    memset(&g_mic_ctx, 0, sizeof(g_mic_ctx));
    
    /* Find audio device */
    rt = tdl_audio_find(AUDIO_CODEC_NAME, &g_mic_ctx.audio_hdl);
    if (rt != OPRT_OK) {
        PR_ERR("Failed to find audio codec: %d", rt);
        return rt;
    }
    
    /* Create ring buffer for audio data - use COVERAGE type so old data is overwritten
     * instead of blocking writes. This prevents backpressure affecting the audio driver. */
    rt = tuya_ring_buff_create(MIC_RINGBUF_SIZE, OVERFLOW_PSRAM_COVERAGE_TYPE, &g_mic_ctx.ringbuf);
    if (rt != OPRT_OK) {
        PR_ERR("Failed to create audio ring buffer: %d", rt);
        return rt;
    }
    
    g_mic_ctx.initialized = true;
    PR_INFO("Mic streaming initialized (raw PCM for WebRTC/Opus)");
    PR_INFO("  Sample rate: %d Hz, Frame: %d samples (%d ms)", 
            MIC_SAMPLE_RATE, MIC_FRAME_SAMPLES, MIC_FRAME_MS);
    PR_INFO("  Ring buffer: %d bytes", MIC_RINGBUF_SIZE);
    
    return OPRT_OK;
}

OPERATE_RET mic_streaming_start(const char *host, uint16_t port)
{
    OPERATE_RET rt = OPRT_OK;
    
    if (!g_mic_ctx.initialized) {
        PR_ERR("Mic streaming not initialized");
        return OPRT_INVALID_PARM;
    }
    
    if (g_mic_ctx.streaming) {
        PR_WARN("Mic streaming already active");
        return OPRT_OK;
    }
    
    /* Initialize UDP audio sender */
    rt = udp_audio_init(host, port);
    if (rt != OPRT_OK) {
        PR_ERR("Failed to init UDP audio: %d", rt);
        return rt;
    }
    
    /* Reset ring buffer */
    tuya_ring_buff_reset(g_mic_ctx.ringbuf);
    
    /* Reset stats */
    g_mic_ctx.total_bytes_captured = 0;
    g_mic_ctx.total_frames_sent = 0;
    g_mic_ctx.dropped_frames = 0;
    g_mic_ctx.watchdog_restarts = 0;
    g_mic_ctx.last_send_time = tal_system_get_millisecond();  /* Init time for keepalive */
    
    /* Start streaming flag first */
    g_mic_ctx.streaming = true;
    
    /* Create streaming task */
    THREAD_CFG_T cfg = {
        .stackDepth = MIC_STREAM_TASK_STACK,
        .priority = MIC_STREAM_TASK_PRIO,
        .thrdname = "mic_g711"
    };
    
    rt = tal_thread_create_and_start(&g_mic_ctx.stream_thread, NULL, NULL,
                                      mic_streaming_task, NULL, &cfg);
    if (rt != OPRT_OK) {
        PR_ERR("Failed to create mic streaming thread: %d", rt);
        g_mic_ctx.streaming = false;
        udp_audio_close();
        return rt;
    }
    
    /* Create UDP keepalive task */
    THREAD_CFG_T ka_cfg = {
        .stackDepth = UDP_KEEPALIVE_TASK_STACK,
        .priority = THREAD_PRIO_3,  /* Lower priority than streaming */
        .thrdname = "udp_keepalive"
    };
    
    rt = tal_thread_create_and_start(&g_mic_ctx.keepalive_thread, NULL, NULL,
                                      udp_keepalive_task, NULL, &ka_cfg);
    if (rt != OPRT_OK) {
        PR_WARN("Failed to create keepalive thread: %d (not critical)", rt);
        /* Not critical - audio will still work, just NAT may timeout */
    }
    
    PR_NOTICE("Mic streaming started (raw PCM over UDP to %s:%d for WebRTC/Opus)", host, port);
    
    return OPRT_OK;
}

OPERATE_RET mic_streaming_stop(void)
{
    if (!g_mic_ctx.streaming) {
        return OPRT_OK;
    }
    
    /* Stop streaming flag */
    g_mic_ctx.streaming = false;
    
    /* Wait for thread to exit */
    tal_system_sleep(MIC_STREAM_INTERVAL_MS * 3);
    
    if (g_mic_ctx.stream_thread) {
        tal_thread_delete(g_mic_ctx.stream_thread);
        g_mic_ctx.stream_thread = NULL;
    }
    
    /* Close UDP connection */
    udp_audio_close();
    
    /* Clear ring buffer */
    tuya_ring_buff_reset(g_mic_ctx.ringbuf);
    
    PR_NOTICE("Mic streaming stopped");
    PR_NOTICE("  Stats: captured=%u bytes, sent=%u frames, dropped=%u, watchdog_restarts=%u", 
              g_mic_ctx.total_bytes_captured, g_mic_ctx.total_frames_sent, 
              g_mic_ctx.dropped_frames, g_mic_ctx.watchdog_restarts);
    
    return OPRT_OK;
}

bool mic_streaming_is_active(void)
{
    return g_mic_ctx.streaming;
}

void mic_streaming_get_stats(uint32_t *bytes_sent, uint32_t *frames_sent)
{
    if (bytes_sent) {
        *bytes_sent = g_mic_ctx.total_bytes_captured;
    }
    if (frames_sent) {
        *frames_sent = g_mic_ctx.total_frames_sent;
    }
}

void *mic_streaming_get_callback(void)
{
    return (void *)mic_audio_frame_callback;
}
