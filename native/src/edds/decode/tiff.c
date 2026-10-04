#include "memory.h"
/*
 * Baseline TIFF, in the one shape a texture source arrives in.
 *
 * Supported: either byte order, a single IFD, 8 bits per sample, chunky planar configuration, the
 * identity orientation, unsigned samples, no predictor, strips rather than tiles, and one of four
 * compressions — none, LZW, Deflate and PackBits. The channel layout is greyscale BlackIsZero,
 * RGB, or RGB with one unassociated alpha extra sample.
 *
 * Everything else — palette, WhiteIsZero, CMYK, YCbCr, 1/4/16/32-bit samples, planar separation,
 * tiles, a horizontal predictor, premultiplied or unspecified extra samples, a rotated orientation,
 * and every further page — is refused by name. Those are not gaps in a decoder so much as subtypes
 * whose Workbench treatment has not been established, and the wrong guess produces a wrong image
 * rather than an error somebody can act on.
 *
 * Alpha is a fact of the source, never a profile field: it exists only where the file declares an
 * unassociated alpha extra sample.
 */
#include "image.h"

#include <stdlib.h>
#include <string.h>

enum {
    TIFF_TAG_WIDTH                = 256,
    TIFF_TAG_HEIGHT               = 257,
    TIFF_TAG_BITS_PER_SAMPLE      = 258,
    TIFF_TAG_COMPRESSION          = 259,
    TIFF_TAG_PHOTOMETRIC          = 262,
    TIFF_TAG_FILL_ORDER           = 266,
    TIFF_TAG_STRIP_OFFSETS        = 273,
    TIFF_TAG_ORIENTATION          = 274,
    TIFF_TAG_SAMPLES_PER_PIXEL    = 277,
    TIFF_TAG_ROWS_PER_STRIP       = 278,
    TIFF_TAG_STRIP_BYTE_COUNTS    = 279,
    TIFF_TAG_PLANAR_CONFIGURATION = 284,
    TIFF_TAG_PREDICTOR            = 317,
    TIFF_TAG_TILE_WIDTH           = 322,
    TIFF_TAG_TILE_LENGTH          = 323,
    TIFF_TAG_TILE_OFFSETS         = 324,
    TIFF_TAG_TILE_BYTE_COUNTS     = 325,
    TIFF_TAG_EXTRA_SAMPLES        = 338,
    TIFF_TAG_SAMPLE_FORMAT        = 339
};

enum {
    TIFF_COMPRESSION_NONE        = 1,
    TIFF_COMPRESSION_LZW         = 5,
    TIFF_COMPRESSION_DEFLATE     = 8,
    TIFF_COMPRESSION_DEFLATE_OLD = 32946,
    TIFF_COMPRESSION_PACKBITS    = 32773
};

typedef struct tiff_reader {
    const uint8_t *bytes;
    size_t         size;
    int            big_endian;
    const uint8_t *entries;
    uint32_t       entry_count;
} tiff_reader;

static uint32_t tiff_u16(const tiff_reader *reader, const uint8_t *at) {
    return reader->big_endian ? edds_u16be(at) : edds_u16le(at);
}

static uint32_t tiff_u32(const tiff_reader *reader, const uint8_t *at) {
    return reader->big_endian ? edds_u32be(at) : edds_u32le(at);
}

static const uint8_t *tiff_entry_of(const tiff_reader *reader, uint32_t tag) {
    for (uint32_t at = 0; at < reader->entry_count; ++at) {
        const uint8_t *entry = reader->entries + (size_t)at * 12u;
        if (tiff_u16(reader, entry) == tag) {
            return entry;
        }
    }
    return NULL;
}

static uint32_t tiff_entry_count(const tiff_reader *reader, const uint8_t *entry) {
    return tiff_u32(reader, entry + 4);
}

/** One value out of a tag, inline or through its offset, with the bounds checked either way. */
static int tiff_value_of(
    const tiff_reader *reader,
    const uint8_t     *entry,
    uint32_t           index,
    uint32_t          *value) {
    const uint32_t type    = tiff_u16(reader, entry + 2);
    const uint32_t count   = tiff_entry_count(reader, entry);
    const uint32_t element = type == 1u || type == 2u ? 1u : type == 3u ? 2u
        : type == 4u                                                    ? 4u
                                                                        : 0u;
    const uint8_t *base;
    uint64_t       total;
    if (element == 0u || index >= count) {
        return 0;
    }
    total = (uint64_t)count * element;
    if (total <= 4u) {
        base = entry + 8;
    } else {
        const uint32_t offset = tiff_u32(reader, entry + 8);
        if ((uint64_t)offset + total > (uint64_t)reader->size) {
            return 0;
        }
        base = reader->bytes + offset;
    }
    base   += (size_t)index * element;
    *value  = element == 1u ? base[0] : element == 2u ? tiff_u16(reader, base)
                                                      : tiff_u32(reader, base);
    return 1;
}

/** A tag that may be absent, in which case the baseline default stands in for it. */
static int tiff_scalar_or(
    const tiff_reader *reader,
    uint32_t           tag,
    uint32_t           fallback,
    uint32_t          *value) {
    const uint8_t *entry = tiff_entry_of(reader, tag);
    if (entry == NULL) {
        *value = fallback;
        return 1;
    }
    return tiff_value_of(reader, entry, 0, value);
}

static int tiff_packbits(const uint8_t *input, size_t input_size, uint8_t *output, size_t output_size) {
    size_t at      = 0;
    size_t written = 0;
    while (at < input_size && written < output_size) {
        const int control = (int)(int8_t)input[at++];
        if (control >= 0) {
            const size_t run = (size_t)control + 1u;
            if (run > input_size - at || run > output_size - written) {
                return 0;
            }
            memcpy(output + written, input + at, run);
            at      += run;
            written += run;
        } else if (control != -128) {
            const size_t run = (size_t)(1 - control);
            if (at >= input_size || run > output_size - written) {
                return 0;
            }
            memset(output + written, input[at++], run);
            written += run;
        }
    }
    return written == output_size;
}

enum {
    LZW_CLEAR     = 256,
    LZW_END       = 257,
    LZW_FIRST     = 258,
    LZW_CODES     = 4096,
    LZW_MAX_WIDTH = 12
};

typedef struct lzw_dictionary {
    uint16_t prefix[LZW_CODES];
    uint8_t  suffix[LZW_CODES];
    uint8_t  stack[LZW_CODES];
} lzw_dictionary;

/**
 * Materialises one code as bytes. The chain runs from the last byte back to the first, so the
 * stack is filled from its end and the string always sits in `stack[LZW_CODES - length ..]`.
 */
static int lzw_string(lzw_dictionary *dictionary, uint32_t code, uint32_t next, uint32_t *length) {
    uint32_t written = 0;
    while (code >= 256u) {
        if (code >= next || written + 1u >= LZW_CODES) {
            return 0;
        }
        dictionary->stack[LZW_CODES - 1u - written] = dictionary->suffix[code];
        ++written;
        code = dictionary->prefix[code];
    }
    dictionary->stack[LZW_CODES - 1u - written] = (uint8_t)code;
    *length                                     = written + 1u;
    return 1;
}

/** TIFF LZW: MSB-first codes, the early code-width change, and no reliance on a trailing EOI. */
static int tiff_lzw(const uint8_t *input, size_t input_size, uint8_t *output, size_t output_size) {
    lzw_dictionary *dictionary = edds_calloc(1, sizeof *dictionary);
    uint64_t        bit_at     = 0;
    const uint64_t  bits       = (uint64_t)input_size * 8u;
    uint32_t        next       = LZW_FIRST;
    uint32_t        width      = 9;
    uint32_t        previous   = LZW_CODES;
    size_t          written    = 0;
    int             ok         = 0;
    if (dictionary == NULL) {
        return 0;
    }
    while (bit_at + width <= bits) {
        uint32_t code   = 0;
        uint32_t length = 0;
        for (uint32_t bit = 0; bit < width; ++bit) {
            const uint64_t position = bit_at + bit;
            code                    = (code << 1) |
                ((input[position / 8u] >> (7u - (uint32_t)(position % 8u))) & 1u);
        }
        bit_at += width;
        if (code == LZW_END) {
            ok = written == output_size;
            break;
        }
        if (code == LZW_CLEAR) {
            next     = LZW_FIRST;
            width    = 9;
            previous = LZW_CODES;
            continue;
        }
        if (previous == LZW_CODES) {
            if (code >= 256u) {
                break;
            }
            dictionary->stack[LZW_CODES - 1u] = (uint8_t)code;
            length                            = 1;
        } else if (code < next) {
            if (!lzw_string(dictionary, code, next, &length)) {
                break;
            }
        } else if (code == next) {
            /*
             * The encoder used an entry it was in the middle of defining: the string is the last
             * one again with its own first byte appended, which is the only way that can happen.
             */
            uint32_t previous_length = 0;
            if (!lzw_string(dictionary, previous, next, &previous_length) ||
                previous_length + 1u >= LZW_CODES) {
                break;
            }
            memmove(dictionary->stack + LZW_CODES - previous_length - 1u,
                dictionary->stack + LZW_CODES - previous_length, previous_length);
            dictionary->stack[LZW_CODES - 1u] = dictionary->stack[LZW_CODES - previous_length - 1u];
            length                            = previous_length + 1u;
        } else {
            break;
        }
        if (length > output_size - written) {
            break;
        }
        memcpy(output + written, dictionary->stack + LZW_CODES - length, length);
        written += length;
        if (previous != LZW_CODES) {
            if (next >= LZW_CODES - 1u) {
                break;
            }
            dictionary->prefix[next] = (uint16_t)previous;
            dictionary->suffix[next] = dictionary->stack[LZW_CODES - length];
            ++next;
            if (next + 1u >= (1u << width) && width < LZW_MAX_WIDTH) {
                ++width;
            }
        }
        previous = code;
        if (written == output_size) {
            ok = 1;
            break;
        }
    }
    edds_free(dictionary);
    return ok;
}

static int tiff_decompress(
    uint32_t       compression,
    const uint8_t *input,
    size_t         input_size,
    uint8_t       *output,
    size_t         output_size) {
    switch (compression) {
        case TIFF_COMPRESSION_NONE:
            if (input_size != output_size) {
                return 0;
            }
            memcpy(output, input, output_size);
            return 1;
        case TIFF_COMPRESSION_LZW:
            return tiff_lzw(input, input_size, output, output_size);
        case TIFF_COMPRESSION_DEFLATE:
        case TIFF_COMPRESSION_DEFLATE_OLD:
            return edds_inflate_zlib(input, input_size, output, output_size);
        case TIFF_COMPRESSION_PACKBITS:
            return tiff_packbits(input, input_size, output, output_size);
        default:
            return 0;
    }
}

typedef struct tiff_layout {
    uint32_t width;
    uint32_t height;
    uint32_t samples;
    uint32_t compression;
    uint32_t rows_per_strip;
    uint32_t strips;
    int      has_alpha;
    int      greyscale;
} tiff_layout;

static edds_status tiff_read_layout(
    const tiff_reader *reader,
    tiff_layout       *layout,
    edds_error        *error) {
    const uint8_t *bits_entry;
    const uint8_t *offsets;
    const uint8_t *counts;
    uint32_t       photometric = 0;
    uint32_t       value       = 0;
    if (!tiff_scalar_or(reader, TIFF_TAG_WIDTH, 0, &layout->width) ||
        !tiff_scalar_or(reader, TIFF_TAG_HEIGHT, 0, &layout->height) ||
        !tiff_scalar_or(reader, TIFF_TAG_SAMPLES_PER_PIXEL, 1, &layout->samples) ||
        !tiff_scalar_or(reader, TIFF_TAG_COMPRESSION, TIFF_COMPRESSION_NONE, &layout->compression) ||
        !tiff_scalar_or(reader, TIFF_TAG_PHOTOMETRIC, 0xffffffffu, &photometric)) {
        edds_fail(error, "malformed-tiff-directory", "A TIFF directory entry points outside the input.");
        return EDDS_INVALID_INPUT;
    }
    if (!edds_decoded_size_allowed(layout->width, layout->height)) {
        edds_fail(error, "tiff-dimension-limit",
            "TIFF dimensions must be between 1 and %u and fit the decoded-image limit.",
            EDDS_MAX_DIMENSION);
        return EDDS_INVALID_INPUT;
    }
    if (tiff_entry_of(reader, TIFF_TAG_TILE_WIDTH) != NULL ||
        tiff_entry_of(reader, TIFF_TAG_TILE_LENGTH) != NULL ||
        tiff_entry_of(reader, TIFF_TAG_TILE_OFFSETS) != NULL ||
        tiff_entry_of(reader, TIFF_TAG_TILE_BYTE_COUNTS) != NULL) {
        edds_fail(error, "unsupported-tiff-layout", "Tiled TIFF inputs are not supported; strips are.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    if (!tiff_scalar_or(reader, TIFF_TAG_PLANAR_CONFIGURATION, 1, &value) || value != 1u) {
        edds_fail(error, "unsupported-tiff-layout",
            "Only chunky TIFF planar configuration 1 is supported.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    if (!tiff_scalar_or(reader, TIFF_TAG_FILL_ORDER, 1, &value) || value != 1u) {
        edds_fail(error, "unsupported-tiff-layout", "Only TIFF fill order 1 is supported.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    if (!tiff_scalar_or(reader, TIFF_TAG_PREDICTOR, 1, &value) || value != 1u) {
        edds_fail(error, "unsupported-tiff-predictor",
            "A TIFF horizontal predictor is not supported; only predictor 1 is.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    if (!tiff_scalar_or(reader, TIFF_TAG_ORIENTATION, 1, &value) || value != 1u) {
        edds_fail(error, "unsupported-tiff-orientation",
            "Only the top-left TIFF orientation is supported; this file declares %u.", value);
        return EDDS_UNSUPPORTED_FORMAT;
    }
    if (layout->samples == 0u || layout->samples > 4u) {
        edds_fail(error, "unsupported-tiff-channels",
            "Only 1, 3 and 4 samples per pixel are supported, not %u.", layout->samples);
        return EDDS_UNSUPPORTED_FORMAT;
    }
    bits_entry = tiff_entry_of(reader, TIFF_TAG_BITS_PER_SAMPLE);
    if (bits_entry == NULL || tiff_entry_count(reader, bits_entry) != layout->samples) {
        edds_fail(error, "malformed-tiff-directory",
            "TIFF BitsPerSample is missing or does not describe every sample.");
        return EDDS_INVALID_INPUT;
    }
    for (uint32_t at = 0; at < layout->samples; ++at) {
        if (!tiff_value_of(reader, bits_entry, at, &value)) {
            edds_fail(error, "malformed-tiff-directory", "TIFF BitsPerSample points outside the input.");
            return EDDS_INVALID_INPUT;
        }
        if (value != 8u) {
            edds_fail(error, "unsupported-tiff-bit-depth",
                "Only 8 bits per sample are supported; this file declares %u.", value);
            return EDDS_UNSUPPORTED_FORMAT;
        }
    }
    {
        const uint8_t *format = tiff_entry_of(reader, TIFF_TAG_SAMPLE_FORMAT);
        for (uint32_t at = 0; format != NULL && at < tiff_entry_count(reader, format); ++at) {
            if (!tiff_value_of(reader, format, at, &value) || value != 1u) {
                edds_fail(error, "unsupported-tiff-sample-format",
                    "Only unsigned integer TIFF samples are supported.");
                return EDDS_UNSUPPORTED_FORMAT;
            }
        }
    }
    if (layout->samples == 1u && photometric == 1u) {
        layout->greyscale = 1;
        layout->has_alpha = 0;
    } else if (layout->samples == 3u && photometric == 2u) {
        layout->greyscale = 0;
        layout->has_alpha = 0;
    } else if (layout->samples == 4u && photometric == 2u) {
        const uint8_t *extra = tiff_entry_of(reader, TIFF_TAG_EXTRA_SAMPLES);
        if (extra == NULL || tiff_entry_count(reader, extra) != 1u ||
            !tiff_value_of(reader, extra, 0, &value) || value != 2u) {
            edds_fail(error, "unsupported-tiff-alpha",
                "A fourth TIFF sample is supported only as one unassociated alpha extra sample.");
            return EDDS_UNSUPPORTED_FORMAT;
        }
        layout->greyscale = 0;
        layout->has_alpha = 1;
    } else {
        edds_fail(error, "unsupported-tiff-channels",
            "Only BlackIsZero greyscale and RGB TIFF inputs are supported, not photometric %u over %u samples.",
            photometric, layout->samples);
        return EDDS_UNSUPPORTED_FORMAT;
    }
    if (layout->compression != TIFF_COMPRESSION_NONE &&
        layout->compression != TIFF_COMPRESSION_LZW &&
        layout->compression != TIFF_COMPRESSION_DEFLATE &&
        layout->compression != TIFF_COMPRESSION_DEFLATE_OLD &&
        layout->compression != TIFF_COMPRESSION_PACKBITS) {
        edds_fail(error, "unsupported-tiff-compression",
            "Only uncompressed, LZW, Deflate and PackBits TIFF strips are supported, not compression %u.",
            layout->compression);
        return EDDS_UNSUPPORTED_FORMAT;
    }
    if (!tiff_scalar_or(reader, TIFF_TAG_ROWS_PER_STRIP, 0xffffffffu, &layout->rows_per_strip) ||
        layout->rows_per_strip == 0u) {
        edds_fail(error, "malformed-tiff-directory", "TIFF RowsPerStrip is zero or unreadable.");
        return EDDS_INVALID_INPUT;
    }
    if (layout->rows_per_strip > layout->height) {
        layout->rows_per_strip = layout->height;
    }
    layout->strips = (layout->height + layout->rows_per_strip - 1u) / layout->rows_per_strip;
    offsets        = tiff_entry_of(reader, TIFF_TAG_STRIP_OFFSETS);
    counts         = tiff_entry_of(reader, TIFF_TAG_STRIP_BYTE_COUNTS);
    if (offsets == NULL || counts == NULL ||
        tiff_entry_count(reader, offsets) != layout->strips ||
        tiff_entry_count(reader, counts) != layout->strips) {
        edds_fail(error, "malformed-tiff-directory",
            "TIFF StripOffsets and StripByteCounts must both describe every strip.");
        return EDDS_INVALID_INPUT;
    }
    return EDDS_OK;
}

static edds_status tiff_read_strips(
    const tiff_reader *reader,
    const tiff_layout *layout,
    uint8_t           *samples,
    edds_error        *error) {
    const uint8_t *offsets   = tiff_entry_of(reader, TIFF_TAG_STRIP_OFFSETS);
    const uint8_t *counts    = tiff_entry_of(reader, TIFF_TAG_STRIP_BYTE_COUNTS);
    const size_t   row_bytes = (size_t)layout->width * layout->samples;
    for (uint32_t strip = 0; strip < layout->strips; ++strip) {
        uint32_t offset;
        uint32_t stored;
        uint32_t rows = layout->rows_per_strip;
        if (!tiff_value_of(reader, offsets, strip, &offset) ||
            !tiff_value_of(reader, counts, strip, &stored)) {
            edds_fail(error, "malformed-tiff-directory", "A TIFF strip descriptor is unreadable.");
            return EDDS_INVALID_INPUT;
        }
        if ((uint64_t)offset + stored > (uint64_t)reader->size) {
            edds_fail(error, "truncated-tiff-strip", "A TIFF strip extends beyond the input boundary.");
            return EDDS_INVALID_INPUT;
        }
        if (strip == layout->strips - 1u) {
            rows = layout->height - strip * layout->rows_per_strip;
        }
        if (!tiff_decompress(layout->compression, reader->bytes + offset, stored,
                samples + (size_t)strip * layout->rows_per_strip * row_bytes,
                row_bytes * rows)) {
            edds_fail(error, "malformed-tiff-strip",
                "A TIFF strip does not decompress to the rows its directory declares.");
            return EDDS_INVALID_INPUT;
        }
    }
    return EDDS_OK;
}

edds_status edds_decode_tiff(FILE *input, edds_decoded_source *image, edds_error *error) {
    uint8_t    *file      = NULL;
    size_t      file_size = 0;
    uint8_t    *samples   = NULL;
    uint8_t    *rgba      = NULL;
    tiff_reader reader;
    tiff_layout layout;
    uint32_t    directory;
    edds_status status = EDDS_INVALID_INPUT;

    if (!edds_read_all(input, &file, &file_size, error)) {
        return EDDS_INVALID_INPUT;
    }
    memset(&layout, 0, sizeof layout);
    reader.bytes       = file;
    reader.size        = file_size;
    reader.entries     = NULL;
    reader.entry_count = 0;
    if (file_size < 8u) {
        edds_fail(error, "truncated-tiff-header", "The TIFF header is truncated.");
        goto done;
    }
    if (memcmp(file, "II\x2a\0", 4) == 0) {
        reader.big_endian = 0;
    } else if (memcmp(file, "MM\0\x2a", 4) == 0) {
        reader.big_endian = 1;
    } else {
        edds_fail(error, "invalid-tiff-signature",
            "The TIFF byte order and magic number are not a classic TIFF header.");
        goto done;
    }
    directory = tiff_u32(&reader, file + 4);
    if (directory < 8u || (uint64_t)directory + 2u > (uint64_t)file_size) {
        edds_fail(error, "malformed-tiff-directory", "The TIFF directory offset is outside the input.");
        goto done;
    }
    reader.entry_count = tiff_u16(&reader, file + directory);
    if (reader.entry_count == 0u ||
        (uint64_t)directory + 2u + (uint64_t)reader.entry_count * 12u + 4u > (uint64_t)file_size) {
        edds_fail(error, "malformed-tiff-directory", "The TIFF directory extends beyond the input.");
        goto done;
    }
    reader.entries = file + directory + 2u;
    if (tiff_u32(&reader, reader.entries + (size_t)reader.entry_count * 12u) != 0u) {
        edds_fail(error, "unsupported-tiff-pages",
            "Only a single-page TIFF is supported; this file carries more directories.");
        status = EDDS_UNSUPPORTED_FORMAT;
        goto done;
    }
    status = tiff_read_layout(&reader, &layout, error);
    if (status != EDDS_OK) {
        goto done;
    }
    status  = EDDS_INVALID_INPUT;
    samples = edds_alloc((size_t)layout.width * layout.height * layout.samples);
    rgba    = edds_alloc((size_t)layout.width * layout.height * 4u);
    if (samples == NULL || rgba == NULL) {
        edds_fail(error, "allocation-failed", "Memory for the decoded TIFF could not be allocated.");
        status = EDDS_INTERNAL_FAILURE;
        goto done;
    }
    status = tiff_read_strips(&reader, &layout, samples, error);
    if (status != EDDS_OK) {
        goto done;
    }
    for (size_t pixel = 0; pixel < (size_t)layout.width * layout.height; ++pixel) {
        const uint8_t *source = samples + pixel * layout.samples;
        uint8_t       *target = rgba + pixel * 4u;
        target[0]             = source[0];
        target[1]             = layout.greyscale ? source[0] : source[1];
        target[2]             = layout.greyscale ? source[0] : source[2];
        target[3]             = layout.has_alpha ? source[3] : 255u;
    }
    image->width     = layout.width;
    image->height    = layout.height;
    image->has_alpha = layout.has_alpha;
    image->rgba      = rgba;
    rgba             = NULL;
    status           = EDDS_OK;

done:
    edds_free(file);
    edds_free(samples);
    edds_free(rgba);
    return status;
}
