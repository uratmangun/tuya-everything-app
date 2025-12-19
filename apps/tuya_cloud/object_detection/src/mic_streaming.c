/**
 * @file mic_streaming.c
 * @brief Microphone audio streaming over TCP
 * 
 * Captures audio from the onboard microphone and streams it
 * via TCP to the web application for playback.
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#include "mic_streaming.h"
#include "tcp_client.h"
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
#define MIC_STREAM_INTERVAL_MS  40  /* Send every 40ms (2 frames = 640 bytes) */

/* Message header for audio data - binary format with prefix */
#define AUDIO_MSG_PREFIX    "audio:"  /* 6 bytes */
#define AUDIO_MSG_PREFIX_LEN 6

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
 * @brief Streaming task - reads from ringbuf and sends to TCP
 */
static void mic_streaming_task(void *arg)
{
    uint8_t send_buf[AUDIO_MSG_PREFIX_LEN + MIC_FRAME_SIZE * 2 + 4];  /* prefix + data + padding */
    uint32_t data_len;
    
    PR_INFO("Mic streaming task started");
    
    while (g_mic_ctx.streaming) {
        /* Check how much data is available */
        data_len = tuya_ring_buff_used_size_get(g_mic_ctx.ringbuf);
        
        if (data_len >= MIC_FRAME_SIZE) {
            /* Limit to 2 frames per send to keep latency low */
            if (data_len > MIC_FRAME_SIZE * 2) {
                data_len = MIC_FRAME_SIZE * 2;
            }
            
            /* Read audio data from ring buffer */
            uint32_t read_len = tuya_ring_buff_read(g_mic_ctx.ringbuf, 
                                                     send_buf + AUDIO_MSG_PREFIX_LEN, 
                                                     data_len);
            
            if (read_len > 0) {
                /* Add prefix for audio data identification */
                memcpy(send_buf, AUDIO_MSG_PREFIX, AUDIO_MSG_PREFIX_LEN);
                
                /* Send via TCP as binary with prefix */
                if (tcp_client_is_connected()) {
                    OPERATE_RET rt = tcp_client_send((const char *)send_buf, 
                                                      AUDIO_MSG_PREFIX_LEN + read_len);
                    if (rt == OPRT_OK) {
                        g_mic_ctx.total_bytes_sent += read_len;
                        g_mic_ctx.total_frames_sent++;
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

OPERATE_RET mic_streaming_start(void)
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
    
    /* Reset ring buffer */
    tuya_ring_buff_reset(g_mic_ctx.ringbuf);
    
    /* Register audio callback to start receiving mic data */
    /* Note: The audio device may need to be re-opened with the callback */
    rt = tdl_audio_open(g_mic_ctx.audio_hdl, mic_audio_frame_callback);
    if (rt != OPRT_OK) {
        PR_ERR("Failed to open audio with callback: %d", rt);
        return rt;
    }
    
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
    
    PR_NOTICE("Mic streaming started - sending audio to web app");
    
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
    
    /* Close audio device (stops mic capture) - re-open without callback for speaker only */
    tdl_audio_open(g_mic_ctx.audio_hdl, NULL);
    
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
