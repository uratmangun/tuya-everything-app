/**
 * @file mic_streaming.h
 * @brief Microphone audio streaming over UDP
 * 
 * Captures audio from the onboard microphone and streams it
 * via UDP to the web application for low-latency playback.
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#ifndef __MIC_STREAMING_H__
#define __MIC_STREAMING_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize microphone streaming module
 * 
 * Must be called after audio subsystem is initialized.
 * 
 * @return OPRT_OK on success
 */
OPERATE_RET mic_streaming_init(void);

/**
 * @brief Start microphone streaming via UDP
 * 
 * Enables the onboard microphone and starts sending audio
 * data over UDP to the specified server.
 * 
 * Audio format: PCM 16-bit, 8000Hz, mono
 * 
 * @param host Server IP address for UDP audio
 * @param port UDP port (e.g., 5001)
 * @return OPRT_OK on success
 */
OPERATE_RET mic_streaming_start(const char *host, uint16_t port);

/**
 * @brief Stop microphone streaming
 * 
 * Disables the onboard microphone and stops sending audio data.
 * 
 * @return OPRT_OK on success
 */
OPERATE_RET mic_streaming_stop(void);

/**
 * @brief Check if microphone streaming is active
 * 
 * @return true if streaming, false otherwise
 */
bool mic_streaming_is_active(void);

/**
 * @brief Get microphone streaming statistics
 * 
 * @param bytes_sent Pointer to store bytes sent (can be NULL)
 * @param frames_sent Pointer to store frames sent (can be NULL)
 */
void mic_streaming_get_stats(uint32_t *bytes_sent, uint32_t *frames_sent);

/**
 * @brief Get the audio callback for mic streaming
 * 
 * This callback should be passed to tdl_audio_open() during initial
 * audio device setup. It will forward mic data when streaming is active.
 * 
 * @return Pointer to the mic audio callback function
 */
void *mic_streaming_get_callback(void);

#ifdef __cplusplus
}
#endif

#endif /* __MIC_STREAMING_H__ */
