/*
 * TGA: colour-map type 0, uncompressed true-colour image type 2, 24 or 32 bits. The descriptor
 * byte is read for origin and alpha depth rather than assumed, and anything else is refused.
 */
#include "memory.h"
#include "image.h"

#include <stdlib.h>
#include <string.h>

/**
 * Decodes a whole TGA file into 8-bit RGBA, top row first whatever order the file stores its rows
 * and columns in. On success the caller owns image->rgba.
 */
edds_status edds_decode_tga(FILE *input, edds_decoded_source *image, edds_error *error) {
    /* The whole file. */
    uint8_t *file = NULL;
    size_t   size = 0;

    /* Where the pixel array starts in the file, and its size in bytes. */
    size_t data_at;
    size_t data_bytes;

    /* What the header says: bytes per pixel, alpha bits per pixel, width and height. */
    uint32_t channels;
    uint32_t attributes;
    uint32_t width;
    uint32_t height;

    /* The RGBA result. */
    uint8_t *rgba;

    if (!edds_read_all(input, &file, &size, error)) {
        return EDDS_INVALID_INPUT;
    }

    if (size < 18u) {
        edds_free(file);
        edds_fail(error, "truncated-tga-header", "The TGA header is truncated.");
        return EDDS_INVALID_INPUT;
    }

    /* Byte 1 is the colour-map type and byte 2 the image type. */
    if (file[1] != 0 || file[2] != 2) {
        edds_free(file);
        edds_fail(error, "unsupported-tga-subtype", "Only TGA color-map type 0 and uncompressed true-color image type 2 are supported.");
        return EDDS_UNSUPPORTED_FORMAT;
    }

    width  = edds_u16le(file + 12);
    height = edds_u16le(file + 14);

    if (width == 0 || height == 0 || width > EDDS_MAX_DIMENSION || height > EDDS_MAX_DIMENSION) {
        edds_free(file);
        edds_fail(error, "tga-dimension-limit", "TGA dimensions must be between 1 and %u.", EDDS_MAX_DIMENSION);
        return EDDS_INVALID_INPUT;
    }

    /* Byte 16 is the bits per pixel. */
    if (file[16] != 24 && file[16] != 32) {
        edds_free(file);
        edds_fail(error, "unsupported-tga-bit-depth", "Only 24-bit and 32-bit true-color TGA inputs are supported.");
        return EDDS_UNSUPPORTED_FORMAT;
    }

    /*
     * Byte 17 is the descriptor. Its low four bits are the alpha bits: none for 24-bit pixels, none
     * or eight for 32-bit ones. Its top two bits, the interleaving, must be clear.
     */
    attributes = file[17] & 0x0fu;

    if ((file[17] & 0xc0u) != 0 || (file[16] == 24 && attributes != 0) || (file[16] == 32 && attributes != 0 && attributes != 8)) {
        edds_free(file);
        edds_fail(error, "unsupported-tga-descriptor", "The TGA descriptor must be non-interleaved with zero or eight alpha bits.");
        return EDDS_UNSUPPORTED_FORMAT;
    }

    /* The pixel array follows the header, after as many more bytes as byte 0 says. */
    channels   = file[16] / 8u;
    data_at    = 18u + file[0];
    data_bytes = (size_t)width * height * channels;

    if (data_at > size || data_bytes > size - data_at || (uint64_t)width * height * 4u > EDDS_MAX_PREVIEW_BYTES) {
        edds_free(file);
        edds_fail(error, "truncated-tga-pixels", "The TGA pixel array is truncated or exceeds the decoded-image limit.");
        return EDDS_INVALID_INPUT;
    }

    rgba = edds_alloc((size_t)width * height * 4u);

    if (rgba == NULL) {
        edds_free(file);
        edds_fail(error, "allocation-failed", "Memory for the decoded TGA could not be allocated.");
        return EDDS_INTERNAL_FAILURE;
    }

    /*
     * Each stored pixel is blue, green and red, then at 32 bits a fourth byte: the alpha when the
     * descriptor gives eight alpha bits, ignored (opaque) when it gives none. Bit 5 of the
     * descriptor set means the rows are stored top first, clear bottom first; bit 4 set means each
     * row is stored right to left.
     */
    for (uint32_t stored_y = 0; stored_y < height; ++stored_y) {
        const uint32_t y = (file[17] & 0x20u) != 0 ? stored_y : height - stored_y - 1u;

        for (uint32_t stored_x = 0; stored_x < width; ++stored_x) {
            const uint32_t x         = (file[17] & 0x10u) != 0 ? width - stored_x - 1u : stored_x;
            const size_t   source_at = data_at + ((size_t)stored_y * width + stored_x) * channels;
            const size_t   output_at = ((size_t)y * width + x) * 4u;

            rgba[output_at]      = file[source_at + 2u];
            rgba[output_at + 1u] = file[source_at + 1u];
            rgba[output_at + 2u] = file[source_at];
            rgba[output_at + 3u] = attributes == 8 ? file[source_at + 3u] : 255u;
        }
    }

    edds_free(file);

    /* The RGBA buffer goes to the caller. */
    image->width     = width;
    image->height    = height;
    image->has_alpha = attributes == 8;
    image->rgba      = rgba;

    return EDDS_OK;
}
