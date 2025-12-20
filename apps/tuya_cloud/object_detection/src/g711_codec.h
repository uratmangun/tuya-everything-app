/**
 * @file g711_codec.h
 * @brief G.711 u-law (PCMU) codec for audio compression
 * 
 * G.711 compresses 16-bit PCM to 8-bit u-law, which:
 * - Reduces bandwidth by 50%
 * - Fixes byte alignment issues (1 byte = 1 sample)
 * - Makes stream robust against dropped packets
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#ifndef __G711_CODEC_H__
#define __G711_CODEC_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Encode PCM 16-bit samples to G.711 u-law
 * 
 * @param pcm_in Input PCM 16-bit samples
 * @param pcm_len Number of PCM samples (not bytes!)
 * @param ulaw_out Output u-law buffer (must be at least pcm_len bytes)
 * @return Number of u-law bytes written (equals pcm_len)
 */
size_t g711_encode_ulaw(const int16_t *pcm_in, size_t pcm_len, uint8_t *ulaw_out);

/**
 * @brief Decode G.711 u-law to PCM 16-bit samples
 * 
 * @param ulaw_in Input u-law bytes
 * @param ulaw_len Number of u-law bytes
 * @param pcm_out Output PCM 16-bit buffer (must be at least ulaw_len * 2 bytes)
 * @return Number of PCM samples written (equals ulaw_len)
 */
size_t g711_decode_ulaw(const uint8_t *ulaw_in, size_t ulaw_len, int16_t *pcm_out);

/**
 * @brief Encode a single PCM sample to u-law
 * 
 * @param pcm 16-bit PCM sample
 * @return 8-bit u-law value
 */
uint8_t g711_linear_to_ulaw(int16_t pcm);

/**
 * @brief Decode a single u-law sample to PCM
 * 
 * @param ulaw 8-bit u-law value
 * @return 16-bit PCM sample
 */
int16_t g711_ulaw_to_linear(uint8_t ulaw);

#ifdef __cplusplus
}
#endif

#endif /* __G711_CODEC_H__ */
