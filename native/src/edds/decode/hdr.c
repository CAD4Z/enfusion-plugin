/*
 * Radiance `.hdr` sources, as far as the installed Workbench decoder was captured reading them:
 * `#?RADIANCE` or `#?RGBE`, comment lines, one `FORMAT=32-bit_rle_rgbe`, a blank line, then the
 * `-Y height +X width` resolution and RGBE pixels, flat or in planar scanline runs. The result is
 * linear RGBA32F with alpha 1.
 */
#include "image.h"
#include "memory.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/** Read one bounded, terminated header line without embedded NULs; accept LF or CRLF. */
static int line_of(const uint8_t *data, size_t size, size_t *at, char line[1024]) {
    size_t n = 0;

    while (*at < size && data[*at] != '\n') {
        if (n == 1023 || data[*at] == 0) {
            return 0;
        }

        line[n++] = (char)data[(*at)++];
    }

    if (*at == size) {
        return 0;
    }

    ++*at;

    if (n != 0 && line[n - 1] == '\r') {
        --n;
    }

    line[n] = 0;

    return 1;
}

/** Consume a decimal uint32 dimension, refusing overflow before arithmetic can wrap. */
static int dimension_of(const char **text, uint32_t *value) {
    const char *p = *text;
    uint64_t    n = 0;

    if (*p < '0' || *p > '9') {
        return 0;
    }

    while (*p >= '0' && *p <= '9') {
        n = n * 10u + (uint32_t)(*p++ - '0');

        if (n > UINT32_MAX) {
            return 0;
        }
    }

    *text  = p;
    *value = (uint32_t)n;

    return 1;
}

/** Decode the captured RGBE subtypes to owned RGBA32F; release every allocation on refusal. */
edds_status edds_decode_hdr(FILE *input, edds_decoded_source *image, edds_error *error) {
    /* Owned source bytes, scanline scratch and decoded float samples. */
    uint8_t *data = NULL, *scanline = NULL;
    float   *pixels = NULL;

    /* Bounded header cursor, dimensions and selected stream encoding. */
    size_t   size = 0, at = 0;
    uint32_t width = 0, height = 0;
    char     line[1024];
    int      format = 0, rle = 0;

    /* One refusal path releases all three allocations. */
    edds_status status = EDDS_INVALID_INPUT;
    const char *code = "invalid-hdr", *message = "The Radiance header or pixel stream is malformed or truncated.";

    if (!edds_read_all(input, &data, &size, error)) {
        return EDDS_INVALID_INPUT;
    }

    if (!line_of(data, size, &at, line) || (strcmp(line, "#?RADIANCE") != 0 && strcmp(line, "#?RGBE") != 0)) {
        goto done;
    }

    /* The header up to its blank line: comments, and one FORMAT; any other variable is refused. */
    for (;;) {
        if (!line_of(data, size, &at, line)) {
            goto done;
        }

        if (line[0] == 0) {
            break;
        }

        if (line[0] == '#') {
            continue;
        }

        if (strcmp(line, "FORMAT=32-bit_rle_rgbe") != 0 || format) {
            status  = EDDS_UNSUPPORTED_FORMAT;
            code    = "unsupported-hdr-subtype";
            message = "HDR requires RGBE with one FORMAT=32-bit_rle_rgbe declaration; other header transforms are unsupported.";
            goto done;
        }

        format = 1;
    }

    if (!format || !line_of(data, size, &at, line)) {
        goto done;
    }

    /* The resolution: rows from the top down, each row from left to right. */
    const char *dimensions = line + 3;

    if (strncmp(line, "-Y ", 3) != 0 ||
        !dimension_of(&dimensions, &height) ||
        strncmp(dimensions, " +X ", 4) != 0 ||
        (dimensions += 4, !dimension_of(&dimensions, &width)) ||
        *dimensions != 0) {
        status  = EDDS_UNSUPPORTED_FORMAT;
        code    = "unsupported-hdr-orientation";
        message = "HDR supports only the proven -Y height +X width orientation.";
        goto done;
    }

    /* Both sides within the converter's limit, and the float samples within 64 MiB. */
    if (width == 0 ||
        height == 0 ||
        width > EDDS_MAX_DIMENSION ||
        height > EDDS_MAX_DIMENSION ||
        (uint64_t)width * height * 16u > EDDS_MAX_PREVIEW_BYTES) {
        code    = "image-size-limit";
        message = "HDR dimensions or decoded float allocation exceed the supported limit.";
        goto done;
    }

    pixels   = edds_alloc((size_t)width * height * 16u);
    scanline = edds_alloc((size_t)width * 4u);

    if (pixels == NULL || scanline == NULL) {
        status  = EDDS_INTERNAL_FAILURE;
        code    = "allocation-failed";
        message = "Memory for the HDR source could not be allocated.";
        goto done;
    }

    /* The first scanline selects planar RLE for the whole stream, as Workbench's loader does. */
    rle = width >= 8 && width < 32768 && size - at >= 4 && data[at] == 2 && data[at + 1] == 2 && data[at + 2] < 128;

    for (uint32_t y = 0; y < height; ++y) {
        if (size - at < 4u) {
            goto done;
        }

        if (rle) {
            if (data[at] != 2 || data[at + 1] != 2 || ((uint32_t)data[at + 2] << 8 | data[at + 3]) != width) {
                goto done;
            }

            at += 4;

            for (uint32_t c = 0; c < 4; ++c) {
                uint32_t x = 0;

                while (x < width) {
                    if (at == size) {
                        goto done;
                    }

                    /*
                     * A token above 128 repeats the next byte token - 128 times; any other is
                     * followed by that many literal bytes.
                     */
                    const uint32_t token = data[at++];
                    const uint32_t count = token > 128 ? token - 128 : token;

                    if (count == 0 || count > width - x || size - at < (token > 128 ? 1u : count)) {
                        goto done;
                    }

                    for (uint32_t i = 0; i < count; ++i) {
                        scanline[(x + i) * 4u + c] = data[at + (token > 128 ? 0u : i)];
                    }

                    at += token > 128 ? 1u : count;
                    x  += count;
                }
            }

        } else {
            if (size - at < (size_t)width * 4u) {
                goto done;
            }

            memcpy(scanline, data + at, (size_t)width * 4u);
            at += (size_t)width * 4u;
        }

        /* Mantissas m share the exponent byte e: a channel is m * 2^(e - 136); e = 0 is black. */
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t *rgbe  = scanline + x * 4u;
            const float    scale = rgbe[3] == 0 ? 0.0f : ldexpf(1.0f, (int)rgbe[3] - 136);
            float         *p     = pixels + ((size_t)y * width + x) * 4u;

            for (uint32_t c = 0; c < 3; ++c) {
                p[c] = rgbe[c] * scale;
            }

            p[3] = 1.0f;
        }
    }

    if (at != size) {
        code    = "trailing-hdr-data";
        message = "The HDR has unexpected bytes after its pixel stream.";
        goto done;
    }

    image->width      = width;
    image->height     = height;
    image->float_rgba = pixels;
    pixels            = NULL;
    status            = EDDS_OK;

done:

    edds_free(data);
    edds_free(scanline);
    edds_free(pixels);

    if (status != EDDS_OK) {
        edds_fail(error, code, "%s", message);
    }

    return status;
}
