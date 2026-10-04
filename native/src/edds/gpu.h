#ifndef EDDS_GPU_H
#define EDDS_GPU_H

#include <edds/edds.h>

/**
 * The runtime pixel formats, as bytes. A block format stores whole 4x4 blocks, so a mip narrower
 * or shorter than a block still costs one: the padding is part of the format, not of the image.
 */

/** Bytes one 4x4 block occupies, or 0 when the format stores separate pixels. */
uint32_t edds_gpu_block_bytes(edds_pixel_format format);

/** Bytes one pixel occupies, or 0 when the format stores 4x4 blocks. */
uint32_t edds_gpu_pixel_bytes(edds_pixel_format format);

/** Stored bytes of one mip, or 0 when the size is outside the supported limits. */
uint32_t edds_gpu_mip_bytes(edds_pixel_format format, uint32_t width, uint32_t height);

/**
 * Turns one BGRA mip into `output`, which is exactly `edds_gpu_mip_bytes` long. `quality` is
 * `ConversionQuality` in thousandths: it buys the encoder search effort and never changes which
 * format comes out. A block that runs off the edge of the image repeats the last real column and
 * row, so the padding carries the picture rather than a seam of black.
 */
void edds_gpu_encode(
    edds_pixel_format format,
    uint32_t quality,
    const uint8_t *bgra,
    uint32_t width,
    uint32_t height,
    uint8_t *output);

/**
 * Decodes one stored mip into top-to-bottom RGBA, showing the values the file actually holds: a
 * single-channel format leaves green and blue at zero rather than repeating red across them.
 * Returns 0 when `stored_bytes` is not what the format and size require.
 */
int edds_gpu_decode(
    edds_pixel_format format,
    const uint8_t *stored,
    uint32_t stored_bytes,
    uint32_t width,
    uint32_t height,
    uint8_t *rgba);

#endif
