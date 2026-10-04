/*
 * PNG, the narrow way. Non-interlaced 8-bit RGB and RGBA only: every other IHDR combination is a
 * subtype whose Workbench treatment has not been established, so it is refused rather than guessed.
 */
#include "memory.h"
#include "image.h"

#include <stdlib.h>
#include <string.h>

/**
 * The CRC-32 that closes every chunk, taken over the chunk type and data: the reflected
 * polynomial 0xEDB88320, one bit at a time.
 */
static uint32_t png_crc32(const uint8_t *bytes, size_t size) {
    uint32_t crc = 0xffffffffu;

    for (size_t at = 0; at < size; ++at) {
        crc ^= bytes[at];

        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }

    return ~crc;
}

/**
 * The predictor of filter type 4: of the bytes to the left, above and above-left, the one
 * closest to left + above - above-left. A tie goes to the left byte, then to the one above.
 */
static uint8_t paeth(uint8_t left, uint8_t above, uint8_t upper_left) {
    const int estimate        = (int)left + above - upper_left;
    const int left_distance   = abs(estimate - left);
    const int above_distance  = abs(estimate - above);
    const int corner_distance = abs(estimate - upper_left);

    if (left_distance <= above_distance && left_distance <= corner_distance) {
        return left;
    }

    return above_distance <= corner_distance ? above : upper_left;
}

/**
 * The zlib stream is the IDAT chunks back to back, and they must be consecutive. Their total is
 * read once, at the first of them, and the stream gets one allocation of exactly that size: grown
 * chunk by chunk, a file of thousands of small IDATs would copy the whole stream once per chunk.
 */
static size_t idat_total_from(const uint8_t *file, size_t file_size, size_t at) {
    size_t total = 0;

    while (file_size - at >= 12u) {
        const uint32_t length = edds_u32be(file + at);

        /* The run ends at the first chunk that is not an IDAT, or that does not fit the file. */
        if (memcmp(file + at + 4u, "IDAT", 4) != 0 || (size_t)length > file_size - at - 12u) {
            break;
        }

        total += length;
        at    += 12u + length;
    }

    return total;
}

/**
 * Appends one IDAT chunk's data to the stream. Data that would not fit the capacity counted up
 * front is refused (0) rather than written past the end.
 */
static int append_idat(uint8_t *idat, size_t capacity, size_t *size, const uint8_t *part,
    size_t part_size) {
    if (part_size > capacity - *size) {
        return 0;
    }

    if (part_size != 0) {
        memcpy(idat + *size, part, part_size);
        *size += part_size;
    }

    return 1;
}

/**
 * Decodes a whole PNG file into 8-bit RGBA: checks the chunks one by one, inflates the image data,
 * undoes the scanline filters and widens RGB to RGBA. On success the caller owns image->rgba.
 */
edds_status edds_decode_png(FILE *input, edds_decoded_source *image, edds_error *error) {
    static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };

    /* The whole file, and where the next chunk starts: right after the 8-byte signature. */
    uint8_t *file      = NULL;
    size_t   file_size = 0;
    size_t   at        = 8;

    /* The zlib stream, gathered from the IDAT chunks. */
    uint8_t *idat          = NULL;
    size_t   idat_size     = 0;
    size_t   idat_capacity = 0;

    /* A truecolor image's tRNS names one colour that is fully transparent wherever it occurs. */
    int     keyed  = 0;
    uint8_t key[3] = { 0, 0, 0 };

    /* What IHDR says. */
    uint32_t width    = 0;
    uint32_t height   = 0;
    uint32_t channels = 0;

    /* Which chunks have been met so far: their order is checked as they come. */
    int saw_ihdr   = 0;
    int saw_idat   = 0;
    int ended_idat = 0;
    int saw_iend   = 0;

    /* The inflated scanlines, each led by its filter byte; the unfiltered rows; the RGBA result. */
    uint8_t *filtered = NULL;
    uint8_t *raw      = NULL;
    uint8_t *rgba     = NULL;
    size_t   row_bytes;
    size_t   filtered_size;

    edds_status status = EDDS_INVALID_INPUT;

    if (!edds_read_all(input, &file, &file_size, error)) {
        return EDDS_INVALID_INPUT;
    }

    if (file_size < sizeof signature || memcmp(file, signature, sizeof signature) != 0) {
        edds_fail(error, "invalid-png-signature", "The PNG signature is invalid or truncated.");
        goto done;
    }

    /*
     * The chunks, one by one. Each is a 4-byte length, a 4-byte type, the data and a 4-byte CRC:
     * 12 bytes around the data.
     */
    while (at < file_size) {
        uint32_t       length;
        const uint8_t *type;
        const uint8_t *data;

        if (file_size - at < 12u) {
            edds_fail(error, "truncated-png-chunk", "A PNG chunk header is truncated.");
            goto done;
        }

        length = edds_u32be(file + at);
        type   = file + at + 4u;
        data   = file + at + 8u;

        if ((size_t)length > file_size - at - 12u) {
            edds_fail(error, "truncated-png-chunk",
                "A PNG chunk extends beyond the input boundary.");
            goto done;
        }

        if (png_crc32(type, 4u + length) != edds_u32be(data + length)) {
            edds_fail(error, "invalid-png-crc", "A PNG chunk has an invalid CRC.");
            goto done;
        }

        if (memcmp(type, "IHDR", 4) == 0) {
            /* The header: the first chunk, exactly one, 13 bytes long. */
            if (saw_ihdr || at != 8u || length != 13u) {
                edds_fail(error, "invalid-png-ihdr",
                    "PNG must contain one 13-byte IHDR as its first chunk.");
                goto done;
            }

            saw_ihdr = 1;
            width    = edds_u32be(data);
            height   = edds_u32be(data + 4);

            if (width == 0 ||
                height == 0 ||
                width > EDDS_MAX_DIMENSION ||
                height > EDDS_MAX_DIMENSION) {
                edds_fail(error, "png-dimension-limit", "PNG dimensions must be between 1 and %u.",
                    EDDS_MAX_DIMENSION);
                goto done;
            }

            /*
             * Bytes 8 to 12 are the bit depth, the colour type, and the compression, filter and
             * interlace methods: 8 bits, RGB (2) or RGBA (6), and method 0 for the other three.
             */
            if (data[8] != 8 ||
                (data[9] != 2 && data[9] != 6) ||
                data[10] != 0 ||
                data[11] != 0 ||
                data[12] != 0) {
                edds_fail(error, "unsupported-png-subtype",
                    "Only non-interlaced 8-bit RGB and RGBA PNG inputs are supported.");
                status = EDDS_UNSUPPORTED_FORMAT;
                goto done;
            }

            channels = data[9] == 6 ? 4u : 3u;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            /* The image data: after IHDR, and every IDAT chunk right after the one before. */
            if (!saw_ihdr || ended_idat) {
                edds_fail(error, "invalid-png-idat",
                    "PNG IDAT chunks are missing, out of order, or exceed the input limit.");
                goto done;
            }

            if (!saw_idat) {
                idat_capacity = idat_total_from(file, file_size, at);
                idat          = edds_alloc(idat_capacity == 0 ? 1u : idat_capacity);

                if (idat == NULL) {
                    edds_fail(error, "allocation-failed",
                        "Memory for the PNG image data could not be allocated.");
                    status = EDDS_INTERNAL_FAILURE;
                    goto done;
                }
            }

            if (!append_idat(idat, idat_capacity, &idat_size, data, length)) {
                edds_fail(error, "invalid-png-idat",
                    "PNG IDAT chunks are missing, out of order, or exceed the input limit.");
                goto done;
            }

            saw_idat = 1;
        } else if (memcmp(type, "tRNS", 4) == 0 || memcmp(type, "PLTE", 4) == 0) {
            /*
             * Both come before the image data. A palette on an RGB or RGBA image is only a
             * suggestion for displays that cannot show the image, and changes no pixel. A tRNS on
             * an RGB image keys one colour transparent; its samples are 16-bit, and one above 255
             * matches no 8-bit pixel. RGBA carries its own alpha and has no tRNS of its own.
             */
            if (!saw_ihdr || saw_idat) {
                edds_fail(error, "invalid-png-chunk-order",
                    "A PNG palette or transparency chunk follows the image data.");
                goto done;
            }

            if (type[0] == 't' &&
                channels == 3u &&
                length == 6u &&
                data[0] == 0u &&
                data[2] == 0u &&
                data[4] == 0u) {
                keyed  = 1;
                key[0] = data[1];
                key[1] = data[3];
                key[2] = data[5];
            }
        } else if (memcmp(type, "IEND", 4) == 0) {
            /* The end: empty, and the last bytes of the file. */
            if (!saw_idat || length != 0 || at + 12u != file_size) {
                edds_fail(error, "invalid-png-iend", "PNG must end with one empty IEND chunk.");
                goto done;
            }

            saw_iend = 1;
        } else {
            /*
             * Any other chunk ends the run of IDAT chunks. It is skipped when it is ancillary,
             * which a lowercase first letter (bit 5 set) marks; a critical one is refused.
             */
            if (saw_idat) {
                ended_idat = 1;
            }

            if ((type[0] & 0x20u) == 0) {
                edds_fail(error, "unsupported-png-critical-chunk",
                    "The PNG contains an unsupported critical chunk.");
                status = EDDS_UNSUPPORTED_FORMAT;
                goto done;
            }
        }

        at += 12u + length;
    }

    if (!saw_ihdr || !saw_idat || !saw_iend) {
        edds_fail(error, "incomplete-png", "PNG is missing IHDR, IDAT, or IEND.");
        goto done;
    }

    /*
     * The buffers. An inflated scanline is its filter byte and width * channels bytes; every
     * product is checked to fit size_t, and the RGBA result to stay within the decoded-image limit.
     */
    if ((uint64_t)width * channels > SIZE_MAX ||
        (uint64_t)width * channels + 1u > SIZE_MAX / height) {
        edds_fail(error, "png-size-overflow",
            "The PNG decoded size overflows the supported address space.");
        goto done;
    }

    row_bytes     = (size_t)width * channels;
    filtered_size = (row_bytes + 1u) * height;

    if ((uint64_t)width * height * 4u > EDDS_MAX_PREVIEW_BYTES) {
        edds_fail(error, "png-decoded-size-limit", "The PNG exceeds the decoded-image limit.");
        goto done;
    }

    filtered = edds_alloc(filtered_size);
    raw      = edds_alloc(row_bytes * height);
    rgba     = edds_alloc((size_t)width * height * 4u);

    if (filtered == NULL || raw == NULL || rgba == NULL) {
        edds_fail(error, "allocation-failed", "Memory for the decoded PNG could not be allocated.");
        status = EDDS_INTERNAL_FAILURE;
        goto done;
    }

    /* The stream must inflate to exactly the scanlines, no more and no less. */
    if (!edds_inflate_zlib(idat, idat_size, filtered, filtered_size)) {
        edds_fail(error, "invalid-png-deflate",
            "The PNG IDAT zlib stream is malformed or has the wrong decoded size.");
        goto done;
    }

    /*
     * The scanline filters undone. Each byte was stored as its difference from a prediction made
     * from its neighbours to the left, above and above-left, with 0 for those outside the image:
     * filter 0 predicts nothing, 1 the left byte, 2 the one above, 3 their average, 4 Paeth.
     */
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t  filter  = filtered[(row_bytes + 1u) * y];
        const uint8_t *encoded = filtered + (row_bytes + 1u) * y + 1u;
        uint8_t       *decoded = raw + row_bytes * y;
        const uint8_t *above   = y == 0 ? NULL : decoded - row_bytes;

        if (filter > 4u) {
            edds_fail(error, "unsupported-png-filter",
                "The PNG scanline uses an unknown filter type.");
            goto done;
        }

        for (size_t x = 0; x < row_bytes; ++x) {
            const uint8_t left      = x < channels ? 0 : decoded[x - channels];
            const uint8_t up        = above == NULL ? 0 : above[x];
            const uint8_t corner    = above == NULL || x < channels ? 0 : above[x - channels];
            uint8_t       predictor = 0;

            if (filter == 1) {
                predictor = left;
            } else if (filter == 2) {
                predictor = up;
            } else if (filter == 3) {
                predictor = (uint8_t)(((uint32_t)left + up) / 2u);
            } else if (filter == 4) {
                predictor = paeth(left, up, corner);
            }

            decoded[x] = (uint8_t)(encoded[x] + predictor);
        }
    }

    /* RGB widened to RGBA: opaque, except the pixels that match the tRNS key. */
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t source_at = ((size_t)y * width + x) * channels;
            const size_t output_at = ((size_t)y * width + x) * 4u;

            rgba[output_at]      = raw[source_at];
            rgba[output_at + 1u] = raw[source_at + 1u];
            rgba[output_at + 2u] = raw[source_at + 2u];

            if (channels == 4) {
                rgba[output_at + 3u] = raw[source_at + 3u];
            } else {
                const int transparent = keyed &&
                    raw[source_at] == key[0] &&
                    raw[source_at + 1u] == key[1] &&
                    raw[source_at + 2u] == key[2];

                rgba[output_at + 3u] = transparent ? 0u : 255u;
            }
        }
    }

    /* The RGBA buffer goes to the caller; everything else is freed below. */
    image->width     = width;
    image->height    = height;
    image->has_alpha = channels == 4 || keyed;
    image->rgba      = rgba;

    rgba   = NULL;
    status = EDDS_OK;

done:
    edds_free(file);
    edds_free(idat);
    edds_free(filtered);
    edds_free(raw);
    edds_free(rgba);
    return status;
}
