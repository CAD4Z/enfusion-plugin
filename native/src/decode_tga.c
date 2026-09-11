/*
 * TGA: colour-map type 0, uncompressed true-colour image type 2, 24 or 32 bits. The descriptor
 * byte is read for origin and alpha depth rather than assumed, and anything else is refused.
 */
#include "image.h"

#include <stdlib.h>
#include <string.h>

edds_status edds_decode_tga(FILE *input, edds_decoded_source *image, edds_error *error) {
    uint8_t *file = NULL;
    size_t size = 0;
    size_t data_at;
    size_t data_bytes;
    uint32_t channels;
    uint32_t attributes;
    uint32_t width;
    uint32_t height;
    uint8_t *rgba;

    if (!edds_read_all(input, &file, &size, error)) {
        return EDDS_INVALID_INPUT;
    }
    if (size < 18u) {
        free(file);
        edds_fail(error, "truncated-tga-header", "The TGA header is truncated.");
        return EDDS_INVALID_INPUT;
    }
    if (file[1] != 0 || file[2] != 2) {
        free(file);
        edds_fail(error, "unsupported-tga-subtype",
            "Only TGA color-map type 0 and uncompressed true-color image type 2 are supported.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    width = edds_u16le(file + 12);
    height = edds_u16le(file + 14);
    if (width == 0 || height == 0 || width > EDDS_MAX_DIMENSION || height > EDDS_MAX_DIMENSION) {
        free(file);
        edds_fail(error, "tga-dimension-limit", "TGA dimensions must be between 1 and %u.", EDDS_MAX_DIMENSION);
        return EDDS_INVALID_INPUT;
    }
    if (file[16] != 24 && file[16] != 32) {
        free(file);
        edds_fail(error, "unsupported-tga-bit-depth", "Only 24-bit and 32-bit true-color TGA inputs are supported.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    attributes = file[17] & 0x0fu;
    if ((file[17] & 0xc0u) != 0 || (file[16] == 24 && attributes != 0) ||
        (file[16] == 32 && attributes != 0 && attributes != 8)) {
        free(file);
        edds_fail(error, "unsupported-tga-descriptor",
            "The TGA descriptor must be non-interleaved with zero or eight alpha bits.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    channels = file[16] / 8u;
    data_at = 18u + file[0];
    data_bytes = (size_t)width * height * channels;
    if (data_at > size || data_bytes > size - data_at ||
        (uint64_t)width * height * 4u > EDDS_MAX_PREVIEW_BYTES) {
        free(file);
        edds_fail(error, "truncated-tga-pixels", "The TGA pixel array is truncated or exceeds the decoded-image limit.");
        return EDDS_INVALID_INPUT;
    }
    rgba = malloc((size_t)width * height * 4u);
    if (rgba == NULL) {
        free(file);
        edds_fail(error, "allocation-failed", "Memory for the decoded TGA could not be allocated.");
        return EDDS_INTERNAL_FAILURE;
    }
    for (uint32_t stored_y = 0; stored_y < height; ++stored_y) {
        const uint32_t y = (file[17] & 0x20u) != 0 ? stored_y : height - stored_y - 1u;
        for (uint32_t stored_x = 0; stored_x < width; ++stored_x) {
            const uint32_t x = (file[17] & 0x10u) != 0 ? width - stored_x - 1u : stored_x;
            const size_t source_at = data_at + ((size_t)stored_y * width + stored_x) * channels;
            const size_t output_at = ((size_t)y * width + x) * 4u;
            rgba[output_at] = file[source_at + 2u];
            rgba[output_at + 1u] = file[source_at + 1u];
            rgba[output_at + 2u] = file[source_at];
            rgba[output_at + 3u] = attributes == 8 ? file[source_at + 3u] : 255u;
        }
    }
    free(file);
    image->width = width;
    image->height = height;
    image->has_alpha = attributes == 8;
    image->rgba = rgba;
    return EDDS_OK;
}
