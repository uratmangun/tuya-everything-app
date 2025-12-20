/**
 * @file g711_codec.c
 * @brief G.711 u-law (PCMU) codec implementation
 * 
 * This implements the ITU-T G.711 u-law standard for audio compression.
 * It converts 16-bit PCM audio to 8-bit u-law encoded audio.
 * 
 * Benefits:
 * - 50% bandwidth reduction
 * - 1 byte = 1 sample (no byte alignment issues over UDP)
 * - Robust against packet loss
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#include "g711_codec.h"

/* u-law encoding constants */
#define ULAW_BIAS     0x84   /* Bias for u-law encoding (132) */
#define ULAW_CLIP     32635  /* Max magnitude before clipping */
#define ULAW_MAX      0x7FFF /* Max positive value for 16-bit */

/* u-law segment and quantization tables */
static const int16_t seg_end[8] = {
    0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF
};

/**
 * @brief Search for the segment containing the linear value
 */
static int search(int16_t val, const int16_t *table, int size)
{
    int i;
    for (i = 0; i < size; i++) {
        if (val <= table[i])
            return i;
    }
    return size;
}

uint8_t g711_linear_to_ulaw(int16_t pcm)
{
    int16_t seg;
    uint8_t uval;
    int16_t mask;
    int16_t pcm_val = pcm;

    /* Get the sign bit */
    if (pcm_val < 0) {
        pcm_val = -pcm_val;
        mask = 0x7F;
    } else {
        mask = 0xFF;
    }

    /* Clip the magnitude */
    if (pcm_val > ULAW_CLIP)
        pcm_val = ULAW_CLIP;

    /* Add bias for u-law */
    pcm_val += ULAW_BIAS;

    /* Find the segment */
    seg = search(pcm_val, seg_end, 8);

    /* Combine sign, segment, and quantization bits */
    if (seg >= 8) {
        /* Out of range, return maximum value */
        uval = 0x7F ^ mask;
    } else {
        uval = (seg << 4) | ((pcm_val >> (seg + 3)) & 0x0F);
        uval ^= mask;
    }

    return uval;
}

int16_t g711_ulaw_to_linear(uint8_t ulaw)
{
    int16_t t;

    /* Complement to obtain normal u-law value */
    ulaw = ~ulaw;

    /* Extract and add the bias */
    t = ((ulaw & 0x0F) << 3) + ULAW_BIAS;

    /* Shift by segment */
    t <<= (ulaw & 0x70) >> 4;

    /* Handle sign bit */
    return (ulaw & 0x80) ? (ULAW_BIAS - t) : (t - ULAW_BIAS);
}

size_t g711_encode_ulaw(const int16_t *pcm_in, size_t pcm_len, uint8_t *ulaw_out)
{
    size_t i;
    for (i = 0; i < pcm_len; i++) {
        ulaw_out[i] = g711_linear_to_ulaw(pcm_in[i]);
    }
    return pcm_len;
}

size_t g711_decode_ulaw(const uint8_t *ulaw_in, size_t ulaw_len, int16_t *pcm_out)
{
    size_t i;
    for (i = 0; i < ulaw_len; i++) {
        pcm_out[i] = g711_ulaw_to_linear(ulaw_in[i]);
    }
    return ulaw_len;
}
