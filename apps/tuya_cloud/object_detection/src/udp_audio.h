/**
 * @file udp_audio.h
 * @brief UDP Audio streaming with raw PCM for WebRTC/Opus
 * 
 * Sends raw PCM audio over UDP for server-side Opus encoding and WebRTC streaming.
 * The server handles Opus encoding and WebRTC, providing:
 * - WebRTC jitter buffer for packet loss recovery
 * - Opus compression (~24kbps output)
 * - Native browser playback (no custom decoder needed)
 * 
 * Packet format: [SEQ:1byte][PCM_DATA:640bytes]
 * PCM is 16kHz, 16-bit, mono = 320 samples * 2 bytes = 640 bytes per 20ms frame
 */

#ifndef __UDP_AUDIO_H__
#define __UDP_AUDIO_H__

#include "tuya_cloud_types.h"

/**
 * @brief Initialize UDP audio sender
 * @param host Server IP address
 * @param port Server UDP port (e.g., 5001)
 * @return OPRT_OK on success
 */
OPERATE_RET udp_audio_init(const char *host, uint16_t port);

/**
 * @brief Send raw PCM audio data via UDP (for server-side Opus encoding)
 * 
 * This function sends raw PCM data with a sequence number.
 * The server will encode to Opus and stream via WebRTC.
 * 
 * @param pcm_data PCM 16-bit audio samples
 * @param pcm_samples Number of samples (not bytes!)
 * @return OPRT_OK on success
 */
OPERATE_RET udp_audio_send_pcm(const int16_t *pcm_data, uint32_t pcm_samples);

/**
 * @brief Send raw audio data via UDP (legacy)
 * @param data Raw audio data
 * @param len Data length
 * @return OPRT_OK on success
 */
OPERATE_RET udp_audio_send(const uint8_t *data, uint32_t len);

/**
 * @brief Check if UDP audio is ready
 */
bool udp_audio_is_ready(void);

/**
 * @brief Get current sequence number
 */
uint8_t udp_audio_get_seq(void);

/**
 * @brief Close UDP audio connection
 */
void udp_audio_close(void);

/**
 * @brief Send UDP keepalive ping
 * 
 * Sends a minimal 1-byte ping packet to keep NAT port mapping alive.
 * Call this every ~25 seconds when NOT streaming audio.
 * 
 * @return OPRT_OK on success
 */
OPERATE_RET udp_audio_send_ping(void);

#endif /* __UDP_AUDIO_H__ */

