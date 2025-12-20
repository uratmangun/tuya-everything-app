/**
 * @file udp_audio.h
 * @brief UDP Audio streaming with G.711 encoding
 * 
 * Sends audio encoded as G.711 u-law over UDP for low-latency streaming.
 * G.711 encoding provides:
 * - 50% bandwidth reduction (8-bit vs 16-bit)
 * - Robust packet handling (1 byte = 1 sample, no alignment issues)
 * - Sequence numbers for ordering and jitter detection
 * 
 * Packet format: [SEQ:1byte][G711_DATA:Nbytes]
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
 * @brief Send raw PCM audio data via UDP (with G.711 encoding)
 * 
 * This function encodes the PCM data to G.711 u-law, adds a sequence
 * number, and sends the packet via UDP.
 * 
 * @param pcm_data PCM 16-bit audio samples
 * @param pcm_samples Number of samples (not bytes!)
 * @return OPRT_OK on success
 */
OPERATE_RET udp_audio_send_pcm(const int16_t *pcm_data, uint32_t pcm_samples);

/**
 * @brief Send raw audio data via UDP (legacy, no encoding)
 * @param data Raw audio data
 * @param len Data length
 * @return OPRT_OK on success
 * @deprecated Use udp_audio_send_pcm() for proper G.711 encoding
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

