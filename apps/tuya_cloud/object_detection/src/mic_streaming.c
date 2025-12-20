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
/* Audio configuration - matching the devkit's onboard mic */
#define MIC_SAMPLE_RATE     8000
#define MIC_BITS            16
#define MIC_CHANNELS        1
#define MIC_FRAME_MS        20
#define MIC_FRAME_SIZE      (MIC_SAMPLE_RATE * MIC_BITS / 8 * MIC_CHANNELS * MIC_FRAME_MS / 1000)  /* 320 bytes per 20ms frame */

/* Ring buffer for audio data - 500ms buffer */
#define MIC_RINGBUF_SIZE    (MIC_FRAME_SIZE * 25)  /* 8000 bytes = 500ms of audio */

/* Streaming task configuration */
#define MIC_STREAM_TASK_STACK   4096
#define MIC_STREAM_TASK_PRIO    THREAD_PRIO_2
#define MIC_STREAM_INTERVAL_MS  20  /* Send every 20ms for lower latency */

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
    
    /* Write to ring buffer (will drop oldest data if full) */
    tuya_ring_buff_write(g_mic_ctx.ringbuf, data, len);
}

/**
 * @brief Streaming task - reads from ringbuf and sends via UDP
 */
static void mic_streaming_task(void *arg)
{
    uint8_t send_buf[MIC_FRAME_SIZE * 2];
    uint32_t data_len;
    uint32_t send_count = 0;
    
    PR_INFO("Mic streaming task started (UDP)");
    
    while (g_mic_ctx.streaming) {
        /* Check how much data is available */
        data_len = tuya_ring_buff_used_size_get(g_mic_ctx.ringbuf);
        
        /* Log buffer status periodically */
        if (send_count % 100 == 0) {
            PR_DEBUG("Mic stream: ringbuf=%u bytes, udp_ready=%d", 
                     data_len, udp_audio_is_ready());
        }
        
        if (data_len >= MIC_FRAME_SIZE) {
            /* Send 1 frame at a time for lower latency */
            if (data_len > MIC_FRAME_SIZE) {
                data_len = MIC_FRAME_SIZE;
            }
            
            /* Read audio data from ring buffer */
            uint32_t read_len = tuya_ring_buff_read(g_mic_ctx.ringbuf, 
                                                     send_buf, 
                                                     data_len);
            
            if (read_len > 0 && udp_audio_is_ready()) {
                /* Send via UDP - no prefix needed, raw PCM */
                OPERATE_RET rt = udp_audio_send(send_buf, read_len);
                if (rt == OPRT_OK) {
                    g_mic_ctx.total_bytes_sent += read_len;
                    g_mic_ctx.total_frames_sent++;
                    send_count++;
                    
                    /* Log every 50 sends (~1 second) */
                    if (send_count % 50 == 0) {
                        PR_INFO("Mic UDP sent: %u frames, %u bytes total", 
                                g_mic_ctx.total_frames_sent, g_mic_ctx.total_bytes_sent);
                    }
                }
            }
        }
        
        /* Wait before next send */
        tal_system_sleep(MIC_STREAM_INTERVAL_MS);
    }
    
    PR_INFO("Mic streaming task stopped");
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
    
    /* Create ring buffer for audio data */
    rt = tuya_ring_buff_create(MIC_RINGBUF_SIZE, OVERFLOW_PSRAM_STOP_TYPE, &g_mic_ctx.ringbuf);
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
