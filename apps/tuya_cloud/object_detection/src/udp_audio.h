/**
 * @file udp_audio.h
 * @brief UDP Audio streaming for low-latency mic data
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
 * @brief Send audio data via UDP
 * @param data PCM audio data
 * @param len Data length
 * @return OPRT_OK on success
 */
OPERATE_RET udp_audio_send(const uint8_t *data, uint32_t len);

/**
 * @brief Check if UDP audio is ready
 */
bool udp_audio_is_ready(void);

#endif /* __UDP_AUDIO_H__ */
