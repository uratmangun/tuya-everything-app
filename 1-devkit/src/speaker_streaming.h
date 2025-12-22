/**
 * @file speaker_streaming.h
 * @brief UDP Speaker Streaming Module API
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#ifndef __SPEAKER_STREAMING_H__
#define __SPEAKER_STREAMING_H__

#include "tuya_cloud_types.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize speaker streaming module
 *
 * Creates UDP socket on port 5002 and starts receiver thread.
 * Audio received will be played through the DevKit speaker.
 *
 * @param host VPS server host (IP or hostname) for NAT hole punching
 * @return OPRT_OK on success
 */
OPERATE_RET speaker_streaming_init(const char *host);

/**
 * @brief Stop speaker streaming
 *
 * @return OPRT_OK on success
 */
OPERATE_RET speaker_streaming_stop(void);

/**
 * @brief Check if speaker streaming is active
 *
 * @return true if active
 */
bool speaker_streaming_is_active(void);

/**
 * @brief Get speaker streaming statistics
 *
 * @param packets Output: number of packets received
 * @param bytes Output: total bytes received
 * @param errors Output: number of play errors
 */
void speaker_streaming_get_stats(uint32_t *packets, uint32_t *bytes, uint32_t *errors);

#ifdef __cplusplus
}
#endif

#endif /* __SPEAKER_STREAMING_H__ */
