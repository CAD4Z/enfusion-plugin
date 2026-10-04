#include "memory.h"
/*
 * Baseline sequential JPEG, and only baseline sequential JPEG.
 *
 * The supported subtype is narrow on purpose: SOF0, 8-bit samples, Huffman coding, one scan, and
 * either one greyscale component or three YCbCr components whose luma sampling is 1x1, 2x1, 1x2 or
 * 2x2 over 1x1 chroma. Progressive, arithmetic, lossless, hierarchical, 12-bit, CMYK/YCCK and an
 * EXIF orientation other than the identity are refused by name before a pixel is produced, because
 * their Workbench treatment has not been established and a plausible guess would be a wrong image
 * rather than an honest refusal.
 *
 * JPEG carries no alpha, so the decoded value never declares one; the conversion below it picks
 * BGRX8 for exactly that reason.
 */
#include "image.h"

#include <stdlib.h>
#include <string.h>

enum {
    JPEG_MAX_COMPONENTS = 3,
    JPEG_QUANT_TABLES   = 4,
    JPEG_HUFFMAN_TABLES = 2,
    JPEG_BLOCK_SAMPLES  = 64
};

static const uint8_t zigzag[JPEG_BLOCK_SAMPLES] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

typedef struct jpeg_huffman {
    int      defined;
    uint8_t  counts[17];
    uint8_t  values[256];
    uint32_t total;
    int32_t  min_code[17];
    int32_t  max_code[17];
    uint32_t value_index[17];
} jpeg_huffman;

typedef struct jpeg_component {
    uint8_t  id;
    uint32_t horizontal;
    uint32_t vertical;
    uint32_t quant_table;
    uint32_t dc_table;
    uint32_t ac_table;
    int32_t  dc_prediction;
    uint32_t stride;
    uint32_t rows;
    uint8_t *plane;
} jpeg_component;

typedef struct jpeg_frame {
    uint32_t       width;
    uint32_t       height;
    uint32_t       component_count;
    uint32_t       horizontal_max;
    uint32_t       vertical_max;
    uint32_t       mcus_x;
    uint32_t       mcus_y;
    jpeg_component components[JPEG_MAX_COMPONENTS];
} jpeg_frame;

/** Entropy-coded bytes with JPEG's own stuffing removed, stopping dead at the next real marker. */
typedef struct jpeg_entropy {
    const uint8_t *bytes;
    size_t         size;
    size_t         at;
    uint32_t       bits;
    unsigned       count;
    int            ended;
} jpeg_entropy;

/** cos(index * pi / 16) for any index, folded from one quarter turn so the table stays exact. */
static double jpeg_cosine(uint32_t index) {
    static const double quarter[9] = {
        1.0,
        0.98078528040323044,
        0.92387953251128674,
        0.83146961230254524,
        0.70710678118654752,
        0.55557023301960222,
        0.38268343236508977,
        0.19509032201612827,
        0.0
    };
    const uint32_t folded = index % 32u;
    if (folded <= 8u) {
        return quarter[folded];
    }
    if (folded <= 16u) {
        return -quarter[16u - folded];
    }
    if (folded <= 24u) {
        return -quarter[folded - 16u];
    }
    return quarter[32u - folded];
}

/** The separable IDCT basis, built once per image rather than per block or at every cosine. */
static void jpeg_basis(double basis[8][8]) {
    for (uint32_t frequency = 0; frequency < 8u; ++frequency) {
        const double weight = frequency == 0u ? 0.35355339059327376 : 0.5;
        for (uint32_t sample = 0; sample < 8u; ++sample) {
            basis[frequency][sample] =
                weight * jpeg_cosine((2u * sample + 1u) * frequency);
        }
    }
}

static void jpeg_idct(
    const int32_t block[JPEG_BLOCK_SAMPLES],
    double        basis[8][8],
    uint8_t      *output,
    size_t        stride) {
    double rows[JPEG_BLOCK_SAMPLES];
    for (uint32_t y = 0; y < 8u; ++y) {
        for (uint32_t x = 0; x < 8u; ++x) {
            double sum = 0.0;
            for (uint32_t frequency = 0; frequency < 8u; ++frequency) {
                sum += basis[frequency][x] * (double)block[y * 8u + frequency];
            }
            rows[y * 8u + x] = sum;
        }
    }
    for (uint32_t x = 0; x < 8u; ++x) {
        for (uint32_t y = 0; y < 8u; ++y) {
            double sum = 0.0;
            long   rounded;
            for (uint32_t frequency = 0; frequency < 8u; ++frequency) {
                sum += basis[frequency][y] * rows[frequency * 8u + x];
            }
            rounded = (long)(sum + (sum < 0.0 ? -0.5 : 0.5)) + 128;
            if (rounded < 0) {
                rounded = 0;
            }
            if (rounded > 255) {
                rounded = 255;
            }
            output[y * stride + x] = (uint8_t)rounded;
        }
    }
}

static int jpeg_build_huffman(jpeg_huffman *table) {
    uint32_t code  = 0;
    uint32_t index = 0;
    for (uint32_t bits = 1; bits <= 16u; ++bits) {
        table->value_index[bits]  = index;
        table->min_code[bits]     = (int32_t)code;
        code                     += table->counts[bits];
        index                    += table->counts[bits];
        if (code > (1u << bits)) {
            return 0;
        }
        table->max_code[bits]   = table->counts[bits] == 0u ? -1 : (int32_t)(code - 1u);
        code                  <<= 1;
    }
    table->total   = index;
    table->defined = 1;
    return 1;
}

static int jpeg_fill(jpeg_entropy *reader) {
    uint8_t byte;
    if (reader->ended) {
        return 0;
    }
    if (reader->at >= reader->size) {
        reader->ended = 1;
        return 0;
    }
    byte = reader->bytes[reader->at];
    if (byte == 0xffu) {
        size_t probe = reader->at + 1u;
        while (probe < reader->size && reader->bytes[probe] == 0xffu) {
            ++probe;
        }
        if (probe >= reader->size || reader->bytes[probe] != 0x00u) {
            /* A real marker. Leave the reader standing on its introducing 0xFF. */
            reader->ended = 1;
            reader->at    = probe >= reader->size ? reader->size : probe - 1u;
            return 0;
        }
        reader->at = probe + 1u;
    } else {
        reader->at += 1u;
    }
    reader->bits   = (reader->bits << 8) | byte;
    reader->count += 8u;
    return 1;
}

static int jpeg_take_bits(jpeg_entropy *reader, unsigned count, uint32_t *value) {
    if (count == 0u) {
        *value = 0;
        return 1;
    }
    while (reader->count < count) {
        if (!jpeg_fill(reader)) {
            return 0;
        }
    }
    *value         = (reader->bits >> (reader->count - count)) & ((1u << count) - 1u);
    reader->count -= count;
    return 1;
}

static int jpeg_decode_symbol(jpeg_entropy *reader, const jpeg_huffman *table, uint8_t *symbol) {
    int32_t code = 0;
    if (!table->defined) {
        return 0;
    }
    for (uint32_t bits = 1; bits <= 16u; ++bits) {
        uint32_t bit;
        uint32_t index;
        if (!jpeg_take_bits(reader, 1u, &bit)) {
            return 0;
        }
        code = (code << 1) | (int32_t)bit;
        if (table->max_code[bits] < 0 || code > table->max_code[bits]) {
            continue;
        }
        index = table->value_index[bits] + (uint32_t)(code - table->min_code[bits]);
        if (index >= table->total) {
            return 0;
        }
        *symbol = table->values[index];
        return 1;
    }
    return 0;
}

static int jpeg_receive_extend(jpeg_entropy *reader, unsigned length, int32_t *value) {
    uint32_t raw;
    if (length == 0u) {
        *value = 0;
        return 1;
    }
    if (length > 16u || !jpeg_take_bits(reader, length, &raw)) {
        return 0;
    }
    *value = (int32_t)raw;
    if (*value < (int32_t)(1u << (length - 1u))) {
        *value -= (int32_t)((1u << length) - 1u);
    }
    return 1;
}

/** Byte-aligns on the expected RSTn and refuses a scan whose restart markers do not line up. */
static int jpeg_restart(jpeg_entropy *reader, uint32_t index) {
    size_t at     = reader->at;
    size_t marker = at;
    reader->bits  = 0;
    reader->count = 0;
    while (marker < reader->size && reader->bytes[marker] == 0xffu) {
        ++marker;
    }
    if (marker == at || marker >= reader->size ||
        reader->bytes[marker] != (uint8_t)(0xd0u + (index % 8u))) {
        return 0;
    }
    reader->at    = marker + 1u;
    reader->ended = 0;
    return 1;
}

static int jpeg_decode_block(
    jpeg_entropy       *reader,
    jpeg_component     *component,
    const jpeg_huffman *dc,
    const jpeg_huffman *ac,
    const uint16_t     *quant,
    double              basis[8][8],
    uint8_t            *output,
    size_t              stride) {
    int32_t  block[JPEG_BLOCK_SAMPLES] = { 0 };
    uint8_t  symbol;
    int32_t  difference;
    uint32_t at = 1;
    if (!jpeg_decode_symbol(reader, dc, &symbol) || symbol > 16u ||
        !jpeg_receive_extend(reader, symbol, &difference)) {
        return 0;
    }
    component->dc_prediction += difference;
    block[0]                  = component->dc_prediction * (int32_t)quant[0];
    while (at < JPEG_BLOCK_SAMPLES) {
        uint32_t run;
        uint32_t size;
        int32_t  value;
        if (!jpeg_decode_symbol(reader, ac, &symbol)) {
            return 0;
        }
        run  = (uint32_t)symbol >> 4;
        size = (uint32_t)symbol & 0x0fu;
        if (size == 0u) {
            if (run != 15u) {
                break;
            }
            at += 16u;
            continue;
        }
        at += run;
        if (at >= JPEG_BLOCK_SAMPLES || !jpeg_receive_extend(reader, size, &value)) {
            return 0;
        }
        block[zigzag[at]] = value * (int32_t)quant[at];
        ++at;
    }
    jpeg_idct(block, basis, output, stride);
    return 1;
}

/**
 * EXIF is a TIFF stream inside APP1. Only the orientation is read, and only to refuse an image the
 * converter would otherwise write out rotated the wrong way. A block this reader cannot follow is
 * left alone rather than turned into a refusal of its own.
 */
static int jpeg_exif_orientation_supported(const uint8_t *segment, size_t size) {
    int            big_endian;
    uint32_t       ifd;
    uint32_t       entries;
    const uint8_t *tiff;
    size_t         tiff_size;
    if (size < 6u + 8u || memcmp(segment, "Exif\0\0", 6) != 0) {
        return 1;
    }
    tiff      = segment + 6u;
    tiff_size = size - 6u;
    if (memcmp(tiff, "II\x2a\0", 4) == 0) {
        big_endian = 0;
    } else if (memcmp(tiff, "MM\0\x2a", 4) == 0) {
        big_endian = 1;
    } else {
        return 1;
    }
    ifd = big_endian ? edds_u32be(tiff + 4) : edds_u32le(tiff + 4);
    if (ifd < 8u || ifd > tiff_size || tiff_size - ifd < 2u) {
        return 1;
    }
    entries = big_endian ? edds_u16be(tiff + ifd) : edds_u16le(tiff + ifd);
    if ((uint64_t)entries * 12u + 2u > tiff_size - ifd) {
        return 1;
    }
    for (uint32_t at = 0; at < entries; ++at) {
        const uint8_t *entry = tiff + ifd + 2u + (size_t)at * 12u;
        const uint32_t tag   = big_endian ? edds_u16be(entry) : edds_u16le(entry);
        const uint32_t type  = big_endian ? edds_u16be(entry + 2) : edds_u16le(entry + 2);
        const uint32_t count = big_endian ? edds_u32be(entry + 4) : edds_u32le(entry + 4);
        if (tag != 274u || type != 3u || count != 1u) {
            continue;
        }
        return (big_endian ? edds_u16be(entry + 8) : edds_u16le(entry + 8)) == 1u;
    }
    return 1;
}

static edds_status jpeg_read_frame(
    const uint8_t *segment,
    size_t         size,
    jpeg_frame    *frame,
    edds_error    *error) {
    if (size < 6u) {
        edds_fail(error, "malformed-jpeg-frame", "The JPEG frame header is truncated.");
        return EDDS_INVALID_INPUT;
    }
    if (segment[0] != 8u) {
        edds_fail(error, "unsupported-jpeg-precision",
            "Only 8-bit sample precision is supported; this frame declares %u.", segment[0]);
        return EDDS_UNSUPPORTED_FORMAT;
    }
    frame->height          = edds_u16be(segment + 1);
    frame->width           = edds_u16be(segment + 3);
    frame->component_count = segment[5];
    if (frame->component_count != 1u && frame->component_count != 3u) {
        edds_fail(error, "unsupported-jpeg-components",
            "Only greyscale and three-component YCbCr JPEG inputs are supported, not %u components.",
            frame->component_count);
        return EDDS_UNSUPPORTED_FORMAT;
    }
    if (size < 6u + (size_t)frame->component_count * 3u) {
        edds_fail(error, "malformed-jpeg-frame", "The JPEG frame component table is truncated.");
        return EDDS_INVALID_INPUT;
    }
    if (!edds_decoded_size_allowed(frame->width, frame->height)) {
        edds_fail(error, "jpeg-dimension-limit",
            "JPEG dimensions must be between 1 and %u and fit the decoded-image limit.",
            EDDS_MAX_DIMENSION);
        return EDDS_INVALID_INPUT;
    }
    frame->horizontal_max = 1;
    frame->vertical_max   = 1;
    for (uint32_t at = 0; at < frame->component_count; ++at) {
        jpeg_component *component = &frame->components[at];
        const uint8_t  *entry     = segment + 6u + (size_t)at * 3u;
        component->id             = entry[0];
        component->horizontal     = (uint32_t)entry[1] >> 4;
        component->vertical       = (uint32_t)entry[1] & 0x0fu;
        component->quant_table    = entry[2];
        component->dc_prediction  = 0;
        component->plane          = NULL;
        if (component->quant_table >= JPEG_QUANT_TABLES) {
            edds_fail(error, "malformed-jpeg-frame",
                "A JPEG component names quantisation table %u.", component->quant_table);
            return EDDS_INVALID_INPUT;
        }
        for (uint32_t other = 0; other < at; ++other) {
            if (frame->components[other].id == component->id) {
                edds_fail(error, "malformed-jpeg-frame", "A JPEG component identifier repeats.");
                return EDDS_INVALID_INPUT;
            }
        }
        if (component->horizontal > frame->horizontal_max) {
            frame->horizontal_max = component->horizontal;
        }
        if (component->vertical > frame->vertical_max) {
            frame->vertical_max = component->vertical;
        }
    }
    /*
     * The proven sampling matrix: 4:4:4, 4:2:2, 4:4:0 and 4:2:0 over 1x1 chroma. Anything else is
     * a layout whose chroma placement is not established here, so it is named and refused.
     */
    if (frame->components[0].horizontal > 2u || frame->components[0].vertical > 2u ||
        frame->components[0].horizontal == 0u || frame->components[0].vertical == 0u) {
        edds_fail(error, "unsupported-jpeg-sampling",
            "Only 1x1, 2x1, 1x2 and 2x2 luma sampling is supported.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    for (uint32_t at = 1; at < frame->component_count; ++at) {
        if (frame->components[at].horizontal != 1u || frame->components[at].vertical != 1u) {
            edds_fail(error, "unsupported-jpeg-sampling",
                "Only 1x1 chroma sampling is supported alongside the luma component.");
            return EDDS_UNSUPPORTED_FORMAT;
        }
    }
    /*
     * A one-component scan is non-interleaved, and its minimum coded unit is a single block rather
     * than the sampling rectangle. Rather than carry a second block walk for a layout nothing
     * writes, a greyscale frame is only accepted when the two orders agree — at 1x1 sampling.
     */
    if (frame->component_count == 1u &&
        (frame->components[0].horizontal != 1u || frame->components[0].vertical != 1u)) {
        edds_fail(error, "unsupported-jpeg-sampling",
            "A greyscale JPEG is supported only at 1x1 sampling.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    frame->mcus_x = (frame->width + frame->horizontal_max * 8u - 1u) /
        (frame->horizontal_max * 8u);
    frame->mcus_y = (frame->height + frame->vertical_max * 8u - 1u) /
        (frame->vertical_max * 8u);
    return EDDS_OK;
}

static edds_status jpeg_allocate_planes(jpeg_frame *frame, edds_error *error) {
    for (uint32_t at = 0; at < frame->component_count; ++at) {
        jpeg_component *component = &frame->components[at];
        const uint64_t  stride    = (uint64_t)frame->mcus_x * component->horizontal * 8u;
        const uint64_t  rows      = (uint64_t)frame->mcus_y * component->vertical * 8u;
        if (stride * rows > (uint64_t)EDDS_MAX_PREVIEW_BYTES) {
            edds_fail(error, "jpeg-decoded-size-limit",
                "The JPEG component planes exceed the decoded-image limit.");
            return EDDS_INVALID_INPUT;
        }
        component->stride = (uint32_t)stride;
        component->rows   = (uint32_t)rows;
        component->plane  = edds_calloc((size_t)(stride * rows), 1u);
        if (component->plane == NULL) {
            edds_fail(error, "allocation-failed",
                "Memory for the decoded JPEG could not be allocated.");
            return EDDS_INTERNAL_FAILURE;
        }
    }
    return EDDS_OK;
}

static uint8_t jpeg_clamp(double value) {
    const long rounded = (long)(value + (value < 0.0 ? -0.5 : 0.5));
    if (rounded < 0) {
        return 0;
    }
    if (rounded > 255) {
        return 255;
    }
    return (uint8_t)rounded;
}

/**
 * Chroma is upsampled by replication and converted with the full-range JFIF matrix. JPEG is a lossy
 * source, so the contract this is judged against is the channel mapping and a bounded error, not a
 * byte-for-byte match with another decoder's resampling filter.
 */
static void jpeg_resolve_pixels(const jpeg_frame *frame, uint8_t *rgba) {
    for (uint32_t y = 0; y < frame->height; ++y) {
        for (uint32_t x = 0; x < frame->width; ++x) {
            const size_t output_at = ((size_t)y * frame->width + x) * 4u;
            uint8_t      sample[JPEG_MAX_COMPONENTS];
            for (uint32_t at = 0; at < frame->component_count; ++at) {
                const jpeg_component *component = &frame->components[at];
                uint32_t              source_x  = x * component->horizontal / frame->horizontal_max;
                uint32_t              source_y  = y * component->vertical / frame->vertical_max;
                if (source_x >= component->stride) {
                    source_x = component->stride - 1u;
                }
                if (source_y >= component->rows) {
                    source_y = component->rows - 1u;
                }
                sample[at] = component->plane[(size_t)source_y * component->stride + source_x];
            }
            if (frame->component_count == 1u) {
                rgba[output_at]      = sample[0];
                rgba[output_at + 1u] = sample[0];
                rgba[output_at + 2u] = sample[0];
            } else {
                const double luma    = sample[0];
                const double blue    = (double)sample[1] - 128.0;
                const double red     = (double)sample[2] - 128.0;
                rgba[output_at]      = jpeg_clamp(luma + 1.402 * red);
                rgba[output_at + 1u] = jpeg_clamp(luma - 0.344136 * blue - 0.714136 * red);
                rgba[output_at + 2u] = jpeg_clamp(luma + 1.772 * blue);
            }
            rgba[output_at + 3u] = 255u;
        }
    }
}

typedef struct jpeg_state {
    jpeg_frame   frame;
    uint16_t     quant[JPEG_QUANT_TABLES][JPEG_BLOCK_SAMPLES];
    int          quant_defined[JPEG_QUANT_TABLES];
    jpeg_huffman dc[JPEG_HUFFMAN_TABLES];
    jpeg_huffman ac[JPEG_HUFFMAN_TABLES];
    uint32_t     restart_interval;
    int          saw_frame;
    int          saw_scan;
    int          adobe_transform;
} jpeg_state;

static edds_status jpeg_read_quant(jpeg_state *state, const uint8_t *segment, size_t size, edds_error *error) {
    size_t at = 0;
    while (at < size) {
        const uint32_t precision = (uint32_t)segment[at] >> 4;
        const uint32_t slot      = (uint32_t)segment[at] & 0x0fu;
        ++at;
        if (precision != 0u) {
            edds_fail(error, "unsupported-jpeg-quantisation",
                "Only 8-bit quantisation tables are supported in a baseline frame.");
            return EDDS_UNSUPPORTED_FORMAT;
        }
        if (slot >= JPEG_QUANT_TABLES || size - at < JPEG_BLOCK_SAMPLES) {
            edds_fail(error, "malformed-jpeg-quantisation", "A JPEG quantisation table is truncated.");
            return EDDS_INVALID_INPUT;
        }
        for (uint32_t entry = 0; entry < JPEG_BLOCK_SAMPLES; ++entry) {
            state->quant[slot][entry] = segment[at + entry];
        }
        state->quant_defined[slot]  = 1;
        at                         += JPEG_BLOCK_SAMPLES;
    }
    return EDDS_OK;
}

static edds_status jpeg_read_huffman(jpeg_state *state, const uint8_t *segment, size_t size, edds_error *error) {
    size_t at = 0;
    while (at < size) {
        uint32_t      kind;
        uint32_t      slot;
        uint32_t      total = 0;
        jpeg_huffman *table;
        if (size - at < 17u) {
            edds_fail(error, "malformed-jpeg-huffman", "A JPEG Huffman table is truncated.");
            return EDDS_INVALID_INPUT;
        }
        kind = (uint32_t)segment[at] >> 4;
        slot = (uint32_t)segment[at] & 0x0fu;
        if (kind > 1u || slot >= JPEG_HUFFMAN_TABLES) {
            edds_fail(error, "unsupported-jpeg-huffman",
                "A baseline frame may only define DC and AC Huffman tables 0 and 1.");
            return EDDS_UNSUPPORTED_FORMAT;
        }
        table = kind == 0u ? &state->dc[slot] : &state->ac[slot];
        memset(table, 0, sizeof *table);
        for (uint32_t bits = 1; bits <= 16u; ++bits) {
            table->counts[bits]  = segment[at + bits];
            total               += table->counts[bits];
        }
        if (total > 256u || size - at - 17u < total) {
            edds_fail(error, "malformed-jpeg-huffman", "A JPEG Huffman table declares more codes than it carries.");
            return EDDS_INVALID_INPUT;
        }
        memcpy(table->values, segment + at + 17u, total);
        if (!jpeg_build_huffman(table)) {
            edds_fail(error, "malformed-jpeg-huffman", "A JPEG Huffman table is over-subscribed.");
            return EDDS_INVALID_INPUT;
        }
        at += 17u + total;
    }
    return EDDS_OK;
}

static edds_status jpeg_read_scan(
    jpeg_state    *state,
    const uint8_t *segment,
    size_t         size,
    const uint8_t *file,
    size_t         file_size,
    size_t         scan_at,
    size_t        *resume_at,
    edds_error    *error) {
    jpeg_frame  *frame = &state->frame;
    jpeg_entropy reader;
    double       basis[8][8];
    uint32_t     declared;
    uint32_t     named = 0;
    uint64_t     total_mcus;
    if (size < 1u) {
        edds_fail(error, "malformed-jpeg-scan", "The JPEG scan header is truncated.");
        return EDDS_INVALID_INPUT;
    }
    declared = segment[0];
    if (declared != frame->component_count || size < 1u + (size_t)declared * 2u + 3u) {
        edds_fail(error, "unsupported-jpeg-scan",
            "Only a single interleaved scan over every frame component is supported.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    for (uint32_t at = 0; at < declared; ++at) {
        const uint8_t   id        = segment[1u + (size_t)at * 2u];
        const uint8_t   tables    = segment[2u + (size_t)at * 2u];
        jpeg_component *component = NULL;
        for (uint32_t candidate = 0; candidate < frame->component_count; ++candidate) {
            if (frame->components[candidate].id == id) {
                if ((named & (1u << candidate)) != 0u) {
                    edds_fail(error, "malformed-jpeg-scan",
                        "The JPEG scan names the same component twice.");
                    return EDDS_INVALID_INPUT;
                }
                named     |= 1u << candidate;
                component  = &frame->components[candidate];
            }
        }
        if (component == NULL) {
            edds_fail(error, "malformed-jpeg-scan", "The JPEG scan names a component the frame does not declare.");
            return EDDS_INVALID_INPUT;
        }
        component->dc_table = (uint32_t)tables >> 4;
        component->ac_table = (uint32_t)tables & 0x0fu;
        if (component->dc_table >= JPEG_HUFFMAN_TABLES || component->ac_table >= JPEG_HUFFMAN_TABLES) {
            edds_fail(error, "unsupported-jpeg-scan",
                "A baseline scan may only select Huffman tables 0 and 1.");
            return EDDS_UNSUPPORTED_FORMAT;
        }
        if (!state->dc[component->dc_table].defined || !state->ac[component->ac_table].defined ||
            !state->quant_defined[component->quant_table]) {
            edds_fail(error, "malformed-jpeg-scan", "The JPEG scan selects a table the file never defines.");
            return EDDS_INVALID_INPUT;
        }
    }
    if (segment[1u + (size_t)declared * 2u] != 0u ||
        segment[2u + (size_t)declared * 2u] != 63u ||
        segment[3u + (size_t)declared * 2u] != 0u) {
        edds_fail(error, "unsupported-jpeg-scan",
            "Only a full baseline spectral selection with no successive approximation is supported.");
        return EDDS_UNSUPPORTED_FORMAT;
    }

    jpeg_basis(basis);
    reader.bytes = file;
    reader.size  = file_size;
    reader.at    = scan_at;
    reader.bits  = 0;
    reader.count = 0;
    reader.ended = 0;
    total_mcus   = (uint64_t)frame->mcus_x * frame->mcus_y;
    for (uint64_t mcu = 0; mcu < total_mcus; ++mcu) {
        if (state->restart_interval != 0u && mcu != 0u && mcu % state->restart_interval == 0u) {
            if (!jpeg_restart(&reader, (uint32_t)((mcu / state->restart_interval - 1u) % 8u))) {
                edds_fail(error, "malformed-jpeg-scan",
                    "A JPEG restart marker is missing or out of sequence.");
                return EDDS_INVALID_INPUT;
            }
            for (uint32_t at = 0; at < frame->component_count; ++at) {
                frame->components[at].dc_prediction = 0;
            }
        }
        for (uint32_t at = 0; at < frame->component_count; ++at) {
            jpeg_component *component = &frame->components[at];
            for (uint32_t down = 0; down < component->vertical; ++down) {
                for (uint32_t across = 0; across < component->horizontal; ++across) {
                    const uint32_t block_x =
                        (uint32_t)(mcu % frame->mcus_x) * component->horizontal + across;
                    const uint32_t block_y =
                        (uint32_t)(mcu / frame->mcus_x) * component->vertical + down;
                    uint8_t *output = component->plane +
                        (size_t)block_y * 8u * component->stride + (size_t)block_x * 8u;
                    if (!jpeg_decode_block(&reader, component,
                            &state->dc[component->dc_table], &state->ac[component->ac_table],
                            state->quant[component->quant_table], basis, output,
                            component->stride)) {
                        edds_fail(error, "truncated-jpeg-scan",
                            "The JPEG entropy-coded data ends before the image does.");
                        return EDDS_INVALID_INPUT;
                    }
                }
            }
        }
    }
    while (!reader.ended) {
        if (!jpeg_fill(&reader)) {
            break;
        }
    }
    *resume_at = reader.at;
    return EDDS_OK;
}

edds_status edds_decode_jpeg(FILE *input, edds_decoded_source *image, edds_error *error) {
    uint8_t    *file      = NULL;
    size_t      file_size = 0;
    size_t      at        = 2;
    uint8_t    *rgba      = NULL;
    jpeg_state *state     = NULL;
    edds_status status    = EDDS_INVALID_INPUT;
    int         saw_end   = 0;

    if (!edds_read_all(input, &file, &file_size, error)) {
        return EDDS_INVALID_INPUT;
    }
    state = edds_calloc(1, sizeof *state);
    if (state == NULL) {
        edds_fail(error, "allocation-failed", "Memory for the JPEG decoder could not be allocated.");
        status = EDDS_INTERNAL_FAILURE;
        goto done;
    }
    state->adobe_transform = -1;
    if (file_size < 4u || file[0] != 0xffu || file[1] != 0xd8u) {
        edds_fail(error, "invalid-jpeg-signature", "The JPEG start-of-image marker is missing.");
        goto done;
    }
    while (at < file_size) {
        uint8_t        marker;
        uint32_t       length;
        const uint8_t *segment;
        size_t         payload;
        if (file[at] != 0xffu) {
            edds_fail(error, "malformed-jpeg-marker", "A JPEG marker segment does not start with 0xFF.");
            goto done;
        }
        while (at < file_size && file[at] == 0xffu) {
            ++at;
        }
        if (at >= file_size) {
            edds_fail(error, "truncated-jpeg-marker", "A JPEG marker is truncated.");
            goto done;
        }
        marker = file[at++];
        if (marker == 0xd9u) {
            if (!state->saw_scan) {
                edds_fail(error, "incomplete-jpeg", "The JPEG ends before it carries a scan.");
                goto done;
            }
            saw_end = 1;
            break;
        }
        if (marker == 0x01u || (marker >= 0xd0u && marker <= 0xd7u)) {
            edds_fail(error, "malformed-jpeg-marker", "A standalone JPEG marker appears outside a scan.");
            goto done;
        }
        if (file_size - at < 2u) {
            edds_fail(error, "truncated-jpeg-marker", "A JPEG marker segment has no length.");
            goto done;
        }
        length = edds_u16be(file + at);
        if (length < 2u || (size_t)length > file_size - at) {
            edds_fail(error, "truncated-jpeg-marker", "A JPEG marker segment extends beyond the input.");
            goto done;
        }
        segment  = file + at + 2u;
        payload  = length - 2u;
        at      += length;
        if (marker == 0xc0u) {
            if (state->saw_frame) {
                edds_fail(error, "malformed-jpeg-frame", "The JPEG declares more than one frame.");
                goto done;
            }
            status = jpeg_read_frame(segment, payload, &state->frame, error);
            if (status != EDDS_OK) {
                goto done;
            }
            state->saw_frame = 1;
        } else if (marker == 0xc4u) {
            status = jpeg_read_huffman(state, segment, payload, error);
            if (status != EDDS_OK) {
                goto done;
            }
        } else if (marker == 0xdbu) {
            status = jpeg_read_quant(state, segment, payload, error);
            if (status != EDDS_OK) {
                goto done;
            }
        } else if (marker == 0xddu) {
            if (payload != 2u) {
                edds_fail(error, "malformed-jpeg-restart", "The JPEG restart interval is malformed.");
                status = EDDS_INVALID_INPUT;
                goto done;
            }
            state->restart_interval = edds_u16be(segment);
        } else if (marker == 0xdau) {
            size_t resume_at = 0;
            if (!state->saw_frame) {
                edds_fail(error, "malformed-jpeg-scan", "A JPEG scan precedes its frame header.");
                status = EDDS_INVALID_INPUT;
                goto done;
            }
            if (state->saw_scan) {
                edds_fail(error, "unsupported-jpeg-scan",
                    "Only a single-scan baseline JPEG is supported.");
                status = EDDS_UNSUPPORTED_FORMAT;
                goto done;
            }
            /*
             * Adobe's APP14 transform is the file saying what its three components really carry.
             * Anything but the YCbCr transform is a colour layout with no established treatment.
             */
            if (state->frame.component_count == 3u && state->adobe_transform >= 0 &&
                state->adobe_transform != 1) {
                edds_fail(error, "unsupported-jpeg-colour",
                    "Only YCbCr three-component JPEG data is supported; this file declares Adobe transform %d.",
                    state->adobe_transform);
                status = EDDS_UNSUPPORTED_FORMAT;
                goto done;
            }
            status = jpeg_allocate_planes(&state->frame, error);
            if (status != EDDS_OK) {
                goto done;
            }
            status = jpeg_read_scan(state, segment, payload, file, file_size, at, &resume_at, error);
            if (status != EDDS_OK) {
                goto done;
            }
            state->saw_scan = 1;
            at              = resume_at;
        } else if (marker == 0xe1u) {
            if (!jpeg_exif_orientation_supported(segment, payload)) {
                edds_fail(error, "unsupported-jpeg-orientation",
                    "The JPEG declares an EXIF orientation this converter would not apply.");
                status = EDDS_UNSUPPORTED_FORMAT;
                goto done;
            }
        } else if (marker == 0xeeu) {
            if (payload >= 12u && memcmp(segment, "Adobe", 5) == 0) {
                state->adobe_transform = segment[11];
            }
        } else if (marker == 0xccu) {
            edds_fail(error, "unsupported-jpeg-frame",
                "Arithmetic-coded JPEG is not supported; only Huffman baseline frames are.");
            status = EDDS_UNSUPPORTED_FORMAT;
            goto done;
        } else if (marker >= 0xc1u && marker <= 0xcfu) {
            edds_fail(error, "unsupported-jpeg-frame",
                "Only baseline sequential JPEG (SOF0) is supported; this file uses SOF%u.",
                (unsigned)(marker - 0xc0u));
            status = EDDS_UNSUPPORTED_FORMAT;
            goto done;
        }
    }
    if (!saw_end) {
        edds_fail(error, "incomplete-jpeg", "The JPEG ends before its end-of-image marker.");
        status = EDDS_INVALID_INPUT;
        goto done;
    }
    status = EDDS_INVALID_INPUT;
    rgba   = edds_alloc((size_t)state->frame.width * state->frame.height * 4u);
    if (rgba == NULL) {
        edds_fail(error, "allocation-failed", "Memory for the decoded JPEG could not be allocated.");
        status = EDDS_INTERNAL_FAILURE;
        goto done;
    }
    jpeg_resolve_pixels(&state->frame, rgba);
    image->width     = state->frame.width;
    image->height    = state->frame.height;
    image->has_alpha = 0;
    image->rgba      = rgba;
    rgba             = NULL;
    status           = EDDS_OK;

done:
    edds_free(file);
    edds_free(rgba);
    if (state != NULL) {
        for (uint32_t component = 0; component < JPEG_MAX_COMPONENTS; ++component) {
            edds_free(state->frame.components[component].plane);
        }
        edds_free(state);
    }
    return status;
}
