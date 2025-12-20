/**
 * @file mic_streaming.c
 * @brief Microphone audio streaming over UDP
 * 
 * Captures audio from the onboard microphone and streams it
 * via UDP to the web application for low-latency playback.
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
/* Audio configuration - actual mic runs at ~62KB/s
 * Likely 16kHz 16-bit stereo, but we just stream raw PCM */
#define MIC_SAMPLE_RATE     16000
#define MIC_BITS            16
#define MIC_CHANNELS        1
#define MIC_FRAME_MS        10       /* 10ms frames for lower latency */
#define MIC_FRAME_SIZE      320      /* Send in 320-byte chunks */

/* Ring buffer for audio data - 2 seconds buffer (handles 62KB/s input) */
#define MIC_RINGBUF_SIZE    (64000 * 2)  /* 128KB = ~2 seconds at 62KB/s */

/* Streaming task configuration */
#define MIC_STREAM_TASK_STACK   4096
#define MIC_STREAM_TASK_PRIO    THREAD_PRIO_2
#define MIC_STREAM_INTERVAL_MS  5   /* Send every 5ms for lower latency */

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
    uint32_t total_bytes_sent;
    uint32_t total_frames_sent;
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
    static uint32_t dropped_frames = 0;
    callback_count++;
    
    /* Log every 100 callbacks to confirm mic is working */
    if (callback_count % 100 == 1) {
        PR_DEBUG("Mic callback #%u: streaming=%d, type=%d, len=%u, dropped=%u", 
                 callback_count, g_mic_ctx.streaming, type, len, dropped_frames);
    }
    
    if (!g_mic_ctx.streaming || !g_mic_ctx.ringbuf) {
        /* Track when not streaming */
        if (g_mic_ctx.streaming && !g_mic_ctx.ringbuf) {
            PR_ERR("Ringbuf is NULL but streaming is true!");
        }
        return;
    }
    
    /* Only handle PCM data */
    if (type != TDL_AUDIO_FRAME_FORMAT_PCM) {
        return;
    }
    
    
    /* Write to ring buffer (will drop oldest data if full with stop type) */
    uint32_t written = tuya_ring_buff_write(g_mic_ctx.ringbuf, data, len);
    if (written < len) {
        dropped_frames++;
        if (dropped_frames % 50 == 1) {
            uint32_t used = tuya_ring_buff_used_size_get(g_mic_ctx.ringbuf);
            PR_WARN("Ring buffer full! Dropped %u bytes (total drops: %u, used: %u)", 
                    len - written, dropped_frames, used);
        }
    }
}

/**
 * @brief Streaming task - reads from ringbuf and sends via UDP
 */
static void mic_streaming_task(void *arg)
{
    uint8_t send_buf[MIC_FRAME_SIZE * 2];
    uint32_t data_len;
    uint32_t send_count = 0;
    uint32_t loop_count = 0;
    uint32_t empty_count = 0;
    uint32_t last_heartbeat = 0;
    
    PR_INFO("Mic streaming task started (UDP)");
    
    while (g_mic_ctx.streaming) {
        loop_count++;
        
        /* Heartbeat every 5 seconds to show thread is alive */
        if ((loop_count - last_heartbeat) >= 1000) {  /* 1000 * 5ms = 5 seconds */
            PR_INFO("Mic stream heartbeat: loops=%u, sends=%u, empty=%u, streaming=%d", 
                     loop_count, send_count, empty_count, g_mic_ctx.streaming);
            last_heartbeat = loop_count;
        }
        
        /* Check if ring buffer is valid */
        if (!g_mic_ctx.ringbuf) {
            PR_ERR("Ring buffer is NULL! Exiting streaming task.");
            break;
        }
        
        /* Check how much data is available */
        data_len = tuya_ring_buff_used_size_get(g_mic_ctx.ringbuf);
        
        /* Log buffer status periodically */
        if (send_count % 200 == 0 && send_count > 0) {
            PR_DEBUG("Mic stream: ringbuf=%u bytes, udp_ready=%d", 
                     data_len, udp_audio_is_ready());
        }
        
        /* Send as much data as available, up to 2KB per iteration to keep up with input rate */
        uint32_t bytes_this_iteration = 0;
        while (data_len >= MIC_FRAME_SIZE && bytes_this_iteration < 2048 && udp_audio_is_ready()) {
            empty_count = 0;  /* Reset empty counter */
            
            /* Send 1 frame at a time */
            uint32_t to_send = (data_len > MIC_FRAME_SIZE) ? MIC_FRAME_SIZE : data_len;
            
            /* Read audio data from ring buffer */
            uint32_t read_len = tuya_ring_buff_read(g_mic_ctx.ringbuf, 
                                                     send_buf, 
                                                     to_send);
            
            if (read_len > 0) {
                /* Send via UDP - no prefix needed, raw PCM */
                OPERATE_RET rt = udp_audio_send(send_buf, read_len);
                if (rt == OPRT_OK) {
                    g_mic_ctx.total_bytes_sent += read_len;
                    g_mic_ctx.total_frames_sent++;
                    send_count++;
                    bytes_this_iteration += read_len;
                    
                    /* Log every 200 sends (~1 second at 200Hz) */
                    if (send_count % 200 == 0) {
                        PR_INFO("Mic UDP sent: %u frames, %u bytes total", 
                                g_mic_ctx.total_frames_sent, g_mic_ctx.total_bytes_sent);
                    }
                } else {
                    PR_WARN("UDP send failed: %d (read_len=%u)", rt, read_len);
                    break;  /* Stop trying if UDP fails */
                }
            } else {
                PR_WARN("Ring buffer read returned 0 despite %u bytes available", data_len);
                break;
            }
            
            /* Update data_len for next iteration */
            data_len = tuya_ring_buff_used_size_get(g_mic_ctx.ringbuf);
        }
        
        if (bytes_this_iteration == 0) {
            empty_count++;
            /* Warn if buffer has been empty for too long (5 seconds at 5ms intervals) */
            if (empty_count == 1000) {  /* 1000 * 5ms = 5 seconds */
                PR_WARN("No mic data for 5 seconds! Mic callback may not be running.");
            }
        }
        
        /* Wait before next send */
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
    PR_INFO("Mic streaming initialized (buffer: %d bytes, frame: %d bytes, rate: %d Hz)", 
            MIC_RINGBUF_SIZE, MIC_FRAME_SIZE, MIC_SAMPLE_RATE);
    
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
    
    /* Note: The audio device is already open with our callback from tuya_main.c
     * We just set the streaming flag and the callback will start buffering data */
    
    /* Reset stats */
    g_mic_ctx.total_bytes_sent = 0;
    g_mic_ctx.total_frames_sent = 0;
    
    /* Start streaming flag first */
    g_mic_ctx.streaming = true;
    
    /* Create streaming task */
    THREAD_CFG_T cfg = {
        .stackDepth = MIC_STREAM_TASK_STACK,
        .priority = MIC_STREAM_TASK_PRIO,
        .thrdname = "mic_stream"
    };
    
    rt = tal_thread_create_and_start(&g_mic_ctx.stream_thread, NULL, NULL,
                                      mic_streaming_task, NULL, &cfg);
    if (rt != OPRT_OK) {
        PR_ERR("Failed to create mic streaming thread: %d", rt);
        g_mic_ctx.streaming = false;
        return rt;
    }
    
    PR_NOTICE("Mic streaming started via UDP to %s:%d", host, port);
    
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
    tal_system_sleep(MIC_STREAM_INTERVAL_MS * 2);
    
    if (g_mic_ctx.stream_thread) {
        tal_thread_delete(g_mic_ctx.stream_thread);
        g_mic_ctx.stream_thread = NULL;
    }
    
    /* Note: We don't close/reopen the audio device - it stays open with our callback.
     * The callback checks the streaming flag and ignores data when not streaming. */
    
    /* Clear ring buffer */
    tuya_ring_buff_reset(g_mic_ctx.ringbuf);
    
    PR_NOTICE("Mic streaming stopped - bytes sent: %u, frames: %u", 
              g_mic_ctx.total_bytes_sent, g_mic_ctx.total_frames_sent);
    
    return OPRT_OK;
}

bool mic_streaming_is_active(void)
{
    return g_mic_ctx.streaming;
}

void mic_streaming_get_stats(uint32_t *bytes_sent, uint32_t *frames_sent)
{
    if (bytes_sent) {
        *bytes_sent = g_mic_ctx.total_bytes_sent;
    }
    if (frames_sent) {
        *frames_sent = g_mic_ctx.total_frames_sent;
    }
}

void *mic_streaming_get_callback(void)
{
    return (void *)mic_audio_frame_callback;
}
