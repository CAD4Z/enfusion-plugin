#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct bytes {
    uint8_t *data;
    size_t size;
} bytes;

static uint32_t u32le(const uint8_t *at) {
    return (uint32_t)at[0] |
        ((uint32_t)at[1] << 8) |
        ((uint32_t)at[2] << 16) |
        ((uint32_t)at[3] << 24);
}

static int load(const char *path, bytes *result) {
    FILE *file = fopen(path, "rb");
    long length;
    int read_ok;
    int close_ok;
    result->data = NULL;
    result->size = 0;
    if (file == NULL || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        return 0;
    }
    result->size = (size_t)length;
    result->data = malloc(result->size == 0 ? 1u : result->size);
    if (result->data == NULL) {
        fclose(file);
        return 0;
    }
    read_ok = fread(result->data, 1, result->size, file) == result->size;
    close_ok = fclose(file) == 0;
    if (!read_ok || !close_ok) {
        free(result->data);
        result->data = NULL;
        result->size = 0;
        return 0;
    }
    return 1;
}

static int length(const uint8_t *input, size_t input_size, size_t *at, size_t base, size_t *value) {
    *value = base;
    if (base != 15u) {
        return 1;
    }
    for (;;) {
        uint8_t part;
        if (*at >= input_size) {
            return 0;
        }
        part = input[(*at)++];
        if (*value > SIZE_MAX - part) {
            return 0;
        }
        *value += part;
        if (part != 255u) {
            return 1;
        }
    }
}

static int decode_lz4(const uint8_t *payload, size_t stored, bytes *decoded) {
    size_t input_at = 4;
    size_t output_at = 0;
    int saw_final = 0;
    if (stored < 8u) {
        return 0;
    }
    decoded->size = u32le(payload);
    decoded->data = malloc(decoded->size == 0 ? 1u : decoded->size);
    if (decoded->size == 0 || decoded->data == NULL) {
        return 0;
    }

    while (input_at < stored && !saw_final) {
        uint32_t framed;
        size_t block_end;
        if (stored - input_at < 4u) {
            goto failure;
        }
        framed = u32le(payload + input_at);
        input_at += 4u;
        if ((size_t)(framed & 0x7fffffffu) > stored - input_at) {
            goto failure;
        }
        block_end = input_at + (framed & 0x7fffffffu);
        while (input_at < block_end) {
            const uint8_t token = payload[input_at++];
            size_t literals;
            size_t match;
            uint32_t offset;
            if (!length(payload, block_end, &input_at, token >> 4, &literals) ||
                literals > block_end - input_at || literals > decoded->size - output_at) {
                goto failure;
            }
            memcpy(decoded->data + output_at, payload + input_at, literals);
            input_at += literals;
            output_at += literals;
            if (input_at == block_end) {
                break;
            }
            if (block_end - input_at < 2u) {
                goto failure;
            }
            offset = (uint32_t)payload[input_at] | ((uint32_t)payload[input_at + 1u] << 8);
            input_at += 2u;
            if (offset == 0 || offset > output_at ||
                !length(payload, block_end, &input_at, token & 0x0fu, &match) ||
                match > SIZE_MAX - 4u) {
                goto failure;
            }
            match += 4u;
            if (match > decoded->size - output_at) {
                goto failure;
            }
            for (size_t index = 0; index < match; ++index) {
                decoded->data[output_at] = decoded->data[output_at - offset];
                ++output_at;
            }
        }
        saw_final = (framed & 0x80000000u) != 0;
    }
    if (!saw_final || input_at != stored || output_at != decoded->size) {
        goto failure;
    }
    return 1;

failure:
    free(decoded->data);
    decoded->data = NULL;
    decoded->size = 0;
    return 0;
}

static int selected_mip(
    const bytes *file,
    uint32_t level,
    uint32_t expected_width,
    uint32_t expected_height,
    uint32_t expected_mips,
    const char expected_container[4],
    bytes *decoded
) {
    uint32_t mips;
    uint32_t stored_index;
    size_t table_at = 128u;
    size_t selected_payload = 0;
    size_t complete;
    uint32_t selected_size = 0;
    const uint8_t *selected_descriptor = NULL;

    decoded->data = NULL;
    decoded->size = 0;
    if (file->size < 136u || memcmp(file->data, "DDS ", 4) != 0 ||
        u32le(file->data + 4) != 124u || memcmp(file->data + 36, "ENF1", 4) != 0 ||
        u32le(file->data + 16) != expected_width || u32le(file->data + 12) != expected_height) {
        return 0;
    }
    mips = u32le(file->data + 28);
    if (mips == 0) {
        mips = 1;
    }
    if (mips != expected_mips || level >= mips || mips > 32u ||
        (size_t)mips > (file->size - table_at) / 8u) {
        return 0;
    }
    complete = table_at + (size_t)mips * 8u;
    stored_index = mips - level - 1u;
    for (uint32_t index = 0; index < mips; ++index) {
        const uint8_t *descriptor = file->data + table_at + (size_t)index * 8u;
        const uint32_t stored = u32le(descriptor + 4);
        if ((memcmp(descriptor, "COPY", 4) != 0 && memcmp(descriptor, "LZ4 ", 4) != 0) ||
            stored == 0 || stored > file->size - complete) {
            return 0;
        }
        if (index == stored_index) {
            selected_descriptor = descriptor;
            selected_size = stored;
            selected_payload = complete;
        }
        complete += stored;
    }
    if (complete != file->size || selected_descriptor == NULL ||
        memcmp(selected_descriptor, expected_container, 4) != 0) {
        return 0;
    }
    if (memcmp(selected_descriptor, "COPY", 4) == 0) {
        decoded->data = malloc(selected_size);
        if (decoded->data == NULL) {
            return 0;
        }
        memcpy(decoded->data, file->data + selected_payload, selected_size);
        decoded->size = selected_size;
        return 1;
    }
    return decode_lz4(file->data + selected_payload, selected_size, decoded);
}

/**
 * JPEG is lossy, so its contract is a bounded difference rather than equal bytes. Everything else
 * this reader checks is lossless and is compared exactly.
 */
static int within(const bytes *decoded, const uint8_t *expected, size_t size, int tolerance) {
    if (decoded->size != size) {
        return 0;
    }
    for (size_t at = 0; at < size; ++at) {
        const int difference = (int)decoded->data[at] - (int)expected[at];
        if (difference > tolerance || difference < -tolerance) {
            return 0;
        }
    }
    return 1;
}

/*
 * The GPU formats, decoded again from the block layouts themselves. None of this shares a line
 * with the converter: a codec that agreed with its own mistake would pass every comparison it
 * made against itself, so the oracle owes its own reading of the bits.
 */

typedef struct gpu_format {
    const char *name;
    const char *four_cc;
    uint32_t dxgi;
    uint32_t block_bytes;
    uint32_t pixel_bytes;
    /* Which of R, G, B, A the format really carries, as a mask over the channel index. */
    unsigned channel_mask;
} gpu_format;

static const gpu_format gpu_formats[] = {
    { "BGRA8", "", 0, 0, 4, 0xf },
    { "BGRX8", "", 0, 0, 4, 0x7 },
    { "R8", "DX10", 61, 0, 1, 0x1 },
    { "RG8", "DX10", 49, 0, 2, 0x3 },
    { "DXT1", "DXT1", 0, 8, 0, 0x7 },
    { "DXT5", "DXT5", 0, 16, 0, 0xf },
    { "BC4", "DX10", 80, 8, 0, 0x1 },
    { "BC5", "DX10", 83, 16, 0, 0x3 },
    { "BC7", "DX10", 98, 16, 0, 0xf }
};

static const gpu_format *gpu_format_named(const char *name) {
    for (size_t at = 0; at < sizeof gpu_formats / sizeof gpu_formats[0]; ++at) {
        if (strcmp(gpu_formats[at].name, name) == 0) {
            return &gpu_formats[at];
        }
    }
    return NULL;
}

static uint32_t blocks_across(uint32_t value) {
    return (value + 3u) / 4u;
}

static uint32_t gpu_mip_bytes(const gpu_format *format, uint32_t width, uint32_t height) {
    return format->block_bytes != 0
        ? blocks_across(width) * blocks_across(height) * format->block_bytes
        : width * height * format->pixel_bytes;
}

static uint8_t channel_of_565(uint32_t value, unsigned shift, unsigned bits) {
    const uint32_t part = (value >> shift) & ((1u << bits) - 1u);
    return (uint8_t)((part << (8u - bits)) | (part >> (2u * bits - 8u)));
}

static uint8_t blend(uint32_t low, uint32_t high, uint32_t low_parts, uint32_t high_parts, uint32_t total) {
    return (uint8_t)((low * low_parts + high * high_parts + total / 2u) / total);
}

/* Standing alone the endpoint order picks the layout; inside BC3 it is always four colours. */
static void decode_bc1(const uint8_t block[8], int always_four_colours, uint8_t rgba[64]) {
    const uint32_t first = (uint32_t)block[0] | ((uint32_t)block[1] << 8);
    const uint32_t second = (uint32_t)block[2] | ((uint32_t)block[3] << 8);
    const int four_colours = always_four_colours || first > second;
    uint8_t palette[4][4];
    for (unsigned entry = 0; entry < 2; ++entry) {
        const uint32_t value = entry == 0 ? first : second;
        palette[entry][0] = channel_of_565(value, 11, 5);
        palette[entry][1] = channel_of_565(value, 5, 6);
        palette[entry][2] = channel_of_565(value, 0, 5);
        palette[entry][3] = 255u;
    }
    for (unsigned channel = 0; channel < 3; ++channel) {
        if (four_colours) {
            palette[2][channel] = blend(palette[0][channel], palette[1][channel], 2, 1, 3);
            palette[3][channel] = blend(palette[0][channel], palette[1][channel], 1, 2, 3);
        } else {
            palette[2][channel] = blend(palette[0][channel], palette[1][channel], 1, 1, 2);
            palette[3][channel] = 0;
        }
    }
    palette[2][3] = 255u;
    palette[3][3] = four_colours ? 255u : 0u;
    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        const unsigned index = (block[4u + pixel / 4u] >> ((pixel % 4u) * 2u)) & 3u;
        memcpy(rgba + pixel * 4u, palette[index], 4);
    }
}

static void decode_bc4(const uint8_t block[8], uint8_t values[16]) {
    uint8_t palette[8];
    uint64_t packed = 0;
    palette[0] = block[0];
    palette[1] = block[1];
    if (block[0] > block[1]) {
        for (unsigned entry = 2; entry < 8; ++entry) {
            palette[entry] = blend(block[0], block[1], 8u - entry, entry - 1u, 7);
        }
    } else {
        for (unsigned entry = 2; entry < 6; ++entry) {
            palette[entry] = blend(block[0], block[1], 6u - entry, entry - 1u, 5);
        }
        palette[6] = 0;
        palette[7] = 255;
    }
    for (unsigned byte = 0; byte < 6; ++byte) {
        packed |= (uint64_t)block[2u + byte] << (byte * 8u);
    }
    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        values[pixel] = palette[(packed >> (pixel * 3u)) & 7u];
    }
}

/* The two partition tables this reader needs, written out from the block layout on their own. */
static const uint8_t reference_partitions[64][16] = {
    { 0,0,1,1, 0,0,1,1, 0,0,1,1, 0,0,1,1 }, { 0,0,0,1, 0,0,0,1, 0,0,0,1, 0,0,0,1 },
    { 0,1,1,1, 0,1,1,1, 0,1,1,1, 0,1,1,1 }, { 0,0,0,1, 0,0,1,1, 0,0,1,1, 0,1,1,1 },
    { 0,0,0,0, 0,0,0,1, 0,0,0,1, 0,0,1,1 }, { 0,0,1,1, 0,1,1,1, 0,1,1,1, 1,1,1,1 },
    { 0,0,0,1, 0,0,1,1, 0,1,1,1, 1,1,1,1 }, { 0,0,0,0, 0,0,0,1, 0,0,1,1, 0,1,1,1 },
    { 0,0,0,0, 0,0,0,0, 0,0,0,1, 0,0,1,1 }, { 0,0,1,1, 0,1,1,1, 1,1,1,1, 1,1,1,1 },
    { 0,0,0,0, 0,0,0,1, 0,1,1,1, 1,1,1,1 }, { 0,0,0,0, 0,0,0,0, 0,0,0,1, 0,1,1,1 },
    { 0,0,0,1, 0,1,1,1, 1,1,1,1, 1,1,1,1 }, { 0,0,0,0, 0,0,0,0, 1,1,1,1, 1,1,1,1 },
    { 0,0,0,0, 1,1,1,1, 1,1,1,1, 1,1,1,1 }, { 0,0,0,0, 0,0,0,0, 0,0,0,0, 1,1,1,1 },
    { 0,0,0,0, 1,0,0,0, 1,1,1,0, 1,1,1,1 }, { 0,1,1,1, 0,0,0,1, 0,0,0,0, 0,0,0,0 },
    { 0,0,0,0, 0,0,0,0, 1,0,0,0, 1,1,1,0 }, { 0,1,1,1, 0,0,1,1, 0,0,0,1, 0,0,0,0 },
    { 0,0,1,1, 0,0,0,1, 0,0,0,0, 0,0,0,0 }, { 0,0,0,0, 1,0,0,0, 1,1,0,0, 1,1,1,0 },
    { 0,0,0,0, 0,0,0,0, 1,0,0,0, 1,1,0,0 }, { 0,1,1,1, 0,0,1,1, 0,0,1,1, 0,0,0,1 },
    { 0,0,1,1, 0,0,0,1, 0,0,0,1, 0,0,0,0 }, { 0,0,0,0, 1,0,0,0, 1,0,0,0, 1,1,0,0 },
    { 0,1,1,0, 0,1,1,0, 0,1,1,0, 0,1,1,0 }, { 0,0,1,1, 0,1,1,0, 0,1,1,0, 1,1,0,0 },
    { 0,0,0,1, 0,1,1,1, 1,1,1,0, 1,0,0,0 }, { 0,0,0,0, 1,1,1,1, 1,1,1,1, 0,0,0,0 },
    { 0,1,1,1, 0,0,0,1, 1,0,0,0, 1,1,1,0 }, { 0,0,1,1, 1,0,0,1, 1,0,0,1, 1,1,0,0 },
    { 0,1,0,1, 0,1,0,1, 0,1,0,1, 0,1,0,1 }, { 0,0,0,0, 1,1,1,1, 0,0,0,0, 1,1,1,1 },
    { 0,1,0,1, 1,0,1,0, 0,1,0,1, 1,0,1,0 }, { 0,0,1,1, 0,0,1,1, 1,1,0,0, 1,1,0,0 },
    { 0,0,1,1, 1,1,0,0, 0,0,1,1, 1,1,0,0 }, { 0,1,0,1, 0,1,0,1, 1,0,1,0, 1,0,1,0 },
    { 0,1,1,0, 1,0,0,1, 0,1,1,0, 1,0,0,1 }, { 0,1,0,1, 1,0,1,0, 1,0,1,0, 0,1,0,1 },
    { 0,1,1,1, 0,0,1,1, 1,1,0,0, 1,1,1,0 }, { 0,0,0,1, 0,0,1,1, 1,1,0,0, 1,0,0,0 },
    { 0,0,1,1, 0,0,1,0, 0,1,0,0, 1,1,0,0 }, { 0,0,1,1, 1,0,1,1, 1,1,0,1, 1,1,0,0 },
    { 0,1,1,0, 1,0,0,1, 1,0,0,1, 0,1,1,0 }, { 0,0,1,1, 1,1,0,0, 1,1,0,0, 0,0,1,1 },
    { 0,1,1,0, 0,1,1,0, 1,0,0,1, 1,0,0,1 }, { 0,0,0,0, 0,1,1,0, 0,1,1,0, 0,0,0,0 },
    { 0,1,0,0, 1,1,1,0, 0,1,0,0, 0,0,0,0 }, { 0,0,1,0, 0,1,1,1, 0,0,1,0, 0,0,0,0 },
    { 0,0,0,0, 0,0,1,0, 0,1,1,1, 0,0,1,0 }, { 0,0,0,0, 0,1,0,0, 1,1,1,0, 0,1,0,0 },
    { 0,1,1,0, 1,1,0,0, 1,0,0,1, 0,0,1,1 }, { 0,0,1,1, 0,1,1,0, 1,1,0,0, 1,0,0,1 },
    { 0,1,1,0, 0,0,1,1, 1,0,0,1, 1,1,0,0 }, { 0,0,1,1, 1,0,0,1, 1,1,0,0, 0,1,1,0 },
    { 0,1,1,0, 1,1,0,0, 1,1,0,0, 1,0,0,1 }, { 0,1,1,0, 0,0,1,1, 0,0,1,1, 1,0,0,1 },
    { 0,1,1,1, 1,1,1,0, 1,0,0,0, 0,0,0,1 }, { 0,0,0,1, 1,0,0,0, 1,1,1,0, 0,1,1,1 },
    { 0,0,0,0, 1,1,1,1, 0,0,1,1, 0,0,1,1 }, { 0,0,1,1, 0,0,1,1, 1,1,1,1, 0,0,0,0 },
    { 0,0,1,0, 0,0,1,0, 1,1,1,0, 1,1,1,0 }, { 0,1,0,0, 0,1,0,0, 0,1,1,1, 0,1,1,1 }
};

static const uint8_t reference_anchor[64] = {
    15,15,15,15,15,15,15,15, 15,15,15,15,15,15,15,15,
    15, 2, 8, 2, 2, 8, 8,15,  2, 8, 2, 2, 8, 8, 2, 2,
    15,15, 6, 8, 2, 8,15,15,  2, 8, 2, 2, 2,15,15, 6,
     6, 2, 6, 8,15,15, 2, 2, 15,15,15,15,15, 2, 2,15
};

static uint32_t reference_bits(const uint8_t *block, unsigned *at, unsigned count) {
    uint32_t value = 0;
    for (unsigned bit = 0; bit < count; ++bit) {
        const unsigned position = *at + bit;
        value |= (uint32_t)((block[position >> 3] >> (position & 7u)) & 1u) << bit;
    }
    *at += count;
    return value;
}

static uint8_t reference_expand(uint32_t value, unsigned bits) {
    return bits >= 8 ? (uint8_t)value
        : (uint8_t)((value << (8u - bits)) | (value >> (2u * bits - 8u)));
}

/**
 * Decodes the three BC7 modes this converter writes. A block in any other mode is refused rather
 * than guessed at: an oracle that accepted a block it could not read would prove nothing.
 */
static int decode_bc7(const uint8_t block[16], uint8_t rgba[64]) {
    static const uint8_t weights_two[4] = { 0, 21, 43, 64 };
    static const uint8_t weights_three[8] = { 0, 9, 18, 27, 37, 46, 55, 64 };
    static const uint8_t weights_four[16] = {
        0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 56, 60, 64
    };
    unsigned at = 0;
    unsigned mode = 0;
    unsigned subsets;
    unsigned channels;
    unsigned bits;
    unsigned index_bits;
    unsigned anchor = 0;
    uint32_t partition = 0;
    const uint8_t *weights;
    const uint8_t *membership;
    uint8_t endpoints[4][4];
    uint8_t indices[16];
    uint32_t raw[4][4] = { { 0 } };
    uint32_t p[4] = { 0, 0, 0, 0 };

    while (mode < 8 && reference_bits(block, &at, 1) == 0) {
        ++mode;
    }
    if (mode == 1) {
        subsets = 2; channels = 3; bits = 6; index_bits = 3; weights = weights_three;
    } else if (mode == 6) {
        subsets = 1; channels = 4; bits = 7; index_bits = 4; weights = weights_four;
    } else if (mode == 7) {
        subsets = 2; channels = 4; bits = 5; index_bits = 2; weights = weights_two;
    } else {
        return 0;
    }
    if (subsets == 2) {
        partition = reference_bits(block, &at, 6);
        anchor = reference_anchor[partition];
    }
    membership = subsets == 2 ? reference_partitions[partition] : NULL;
    for (unsigned channel = 0; channel < channels; ++channel) {
        for (unsigned endpoint = 0; endpoint < subsets * 2u; ++endpoint) {
            raw[endpoint][channel] = reference_bits(block, &at, bits);
        }
    }
    /* Mode 1 shares one P-bit across each subset; modes 6 and 7 give every endpoint its own. */
    if (mode == 1) {
        for (unsigned subset = 0; subset < 2; ++subset) {
            const uint32_t shared = reference_bits(block, &at, 1);
            p[subset * 2u] = shared;
            p[subset * 2u + 1u] = shared;
        }
    } else {
        for (unsigned endpoint = 0; endpoint < subsets * 2u; ++endpoint) {
            p[endpoint] = reference_bits(block, &at, 1);
        }
    }
    for (unsigned endpoint = 0; endpoint < subsets * 2u; ++endpoint) {
        for (unsigned channel = 0; channel < 4; ++channel) {
            endpoints[endpoint][channel] = channel >= channels ? 255u
                : reference_expand((raw[endpoint][channel] << 1) | p[endpoint], bits + 1u);
        }
    }
    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        const unsigned count = index_bits -
            (pixel == 0 || (subsets == 2 && pixel == anchor) ? 1u : 0u);
        indices[pixel] = (uint8_t)reference_bits(block, &at, count);
    }
    if (at != 128u) {
        return 0;
    }
    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        const unsigned subset = membership == NULL ? 0u : membership[pixel];
        for (unsigned channel = 0; channel < 4; ++channel) {
            const uint32_t low = endpoints[subset * 2u][channel];
            const uint32_t high = endpoints[subset * 2u + 1u][channel];
            rgba[pixel * 4u + channel] = (uint8_t)((low * (64u - weights[indices[pixel]]) +
                high * weights[indices[pixel]] + 32u) >> 6);
        }
    }
    return 1;
}

static void place_block(
    const uint8_t decoded[64],
    uint32_t width,
    uint32_t height,
    uint32_t left,
    uint32_t top,
    uint8_t *rgba
) {
    for (unsigned row = 0; row < 4 && top + row < height; ++row) {
        for (unsigned column = 0; column < 4 && left + column < width; ++column) {
            memcpy(rgba + (((size_t)(top + row) * width) + left + column) * 4u,
                decoded + (row * 4u + column) * 4u, 4);
        }
    }
}

static int decode_gpu_mip(
    const gpu_format *format,
    const bytes *stored,
    uint32_t width,
    uint32_t height,
    uint8_t *rgba
) {
    if (stored->size != gpu_mip_bytes(format, width, height)) {
        return 0;
    }
    if (format->block_bytes == 0) {
        for (size_t pixel = 0; pixel < (size_t)width * height; ++pixel) {
            const uint8_t *at = stored->data + pixel * format->pixel_bytes;
            uint8_t *out = rgba + pixel * 4u;
            out[1] = 0;
            out[2] = 0;
            out[3] = 255u;
            if (format->pixel_bytes == 4u) {
                /* Four bytes is the stored BGRA order, which is why blue comes off the front. */
                out[0] = at[2];
                out[1] = at[1];
                out[2] = at[0];
                out[3] = format->channel_mask == 0xfu ? at[3] : 255u;
            } else if (format->pixel_bytes == 2u) {
                out[0] = at[0];
                out[1] = at[1];
            } else {
                out[0] = at[0];
            }
        }
        return 1;
    }
    for (uint32_t row = 0; row < blocks_across(height); ++row) {
        for (uint32_t column = 0; column < blocks_across(width); ++column) {
            const uint8_t *block = stored->data +
                ((size_t)row * blocks_across(width) + column) * format->block_bytes;
            uint8_t decoded[64];
            uint8_t channel[16];
            uint8_t second[16];
            memset(decoded, 0, sizeof decoded);
            if (strcmp(format->name, "DXT1") == 0) {
                decode_bc1(block, 0, decoded);
            } else if (strcmp(format->name, "DXT5") == 0) {
                decode_bc1(block + 8, 1, decoded);
                decode_bc4(block, channel);
                for (unsigned pixel = 0; pixel < 16; ++pixel) {
                    decoded[pixel * 4u + 3u] = channel[pixel];
                }
            } else if (strcmp(format->name, "BC4") == 0) {
                decode_bc4(block, channel);
                for (unsigned pixel = 0; pixel < 16; ++pixel) {
                    decoded[pixel * 4u] = channel[pixel];
                    decoded[pixel * 4u + 3u] = 255u;
                }
            } else if (strcmp(format->name, "BC5") == 0) {
                decode_bc4(block, channel);
                decode_bc4(block + 8, second);
                for (unsigned pixel = 0; pixel < 16; ++pixel) {
                    decoded[pixel * 4u] = channel[pixel];
                    decoded[pixel * 4u + 1u] = second[pixel];
                    decoded[pixel * 4u + 3u] = 255u;
                }
            } else if (!decode_bc7(block, decoded)) {
                return 0;
            }
            place_block(decoded, width, height, column * 4u, row * 4u, rgba);
        }
    }
    return 1;
}

/** The nine-by-five source, read back out of the TGA the fixture wrote, as straight RGBA. */
static int expected_source(const bytes *tga, uint32_t *width, uint32_t *height, uint8_t **rgba) {
    size_t pixels;
    size_t stride;
    if (tga->size < 18u || tga->data[2] != 2u ||
        (tga->data[16] != 32u && tga->data[16] != 24u)) {
        return 0;
    }
    stride = tga->data[16] == 32u ? 4u : 3u;
    *width = (uint32_t)tga->data[12] | ((uint32_t)tga->data[13] << 8);
    *height = (uint32_t)tga->data[14] | ((uint32_t)tga->data[15] << 8);
    pixels = (size_t)*width * *height;
    if (tga->size != 18u + pixels * stride) {
        return 0;
    }
    *rgba = malloc(pixels * 4u);
    if (*rgba == NULL) {
        return 0;
    }
    for (size_t pixel = 0; pixel < pixels; ++pixel) {
        const uint8_t *at = tga->data + 18u + pixel * stride;
        (*rgba)[pixel * 4u] = at[2];
        (*rgba)[pixel * 4u + 1u] = at[1];
        (*rgba)[pixel * 4u + 2u] = at[0];
        /* A twenty-four bit TGA declares no alpha, so the source it stands for is opaque. */
        (*rgba)[pixel * 4u + 3u] = stride == 4u ? at[3] : 255u;
    }
    return 1;
}

/**
 * Verifies one GPU conversion of the controlled source: the header names the format DayZ's own
 * textures use for it, every mip is exactly the blocks its dimensions need, the top mip decodes to
 * the source within the bound, and the smallest mip of the chain decodes at all.
 */
static int verify_gpu(const char *source_path, const char *result_path, const char *format_name,
    unsigned bound) {
    bytes source = { NULL, 0 };
    bytes result = { NULL, 0 };
    bytes stored = { NULL, 0 };
    const gpu_format *format = gpu_format_named(format_name);
    uint8_t *expected = NULL;
    uint8_t *decoded = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mips;
    uint32_t header_bytes;
    size_t table_at;
    size_t payload_at;
    uint64_t total = 0;
    unsigned counted = 0;
    int ok = 0;

    if (format == NULL || !load(source_path, &source) || !load(result_path, &result) ||
        !expected_source(&source, &width, &height, &expected)) {
        goto done;
    }
    if (result.size < 128u || memcmp(result.data, "DDS ", 4) != 0 ||
        memcmp(result.data + 36, "ENF1", 4) != 0 ||
        u32le(result.data + 16) != width || u32le(result.data + 12) != height) {
        goto done;
    }
    mips = u32le(result.data + 28);
    header_bytes = 128u;
    if (format->four_cc[0] == '\0') {
        if ((u32le(result.data + 80) & 0x4u) != 0) goto done;
    } else {
        if ((u32le(result.data + 80) & 0x4u) == 0 ||
            memcmp(result.data + 84, format->four_cc, 4) != 0) goto done;
        if (format->dxgi != 0) {
            if (result.size < 148u || u32le(result.data + 128) != format->dxgi ||
                u32le(result.data + 132) != 3u || u32le(result.data + 140) != 1u) goto done;
            header_bytes = 148u;
        }
    }
    /* A block format declares its top mip as a linear size; an uncompressed one as a pitch. */
    if (u32le(result.data + 20) != (format->block_bytes != 0
            ? gpu_mip_bytes(format, width, height) : width * format->pixel_bytes)) {
        goto done;
    }
    if ((u32le(result.data + 8) & 0x80008u) !=
        (format->block_bytes != 0 ? 0x80000u : 0x8u)) {
        goto done;
    }

    table_at = header_bytes;
    payload_at = table_at + (size_t)mips * 8u;
    for (uint32_t index = 0; index < mips; ++index) {
        const uint32_t level = mips - index - 1u;
        uint32_t mip_width = width;
        uint32_t mip_height = height;
        const uint8_t *descriptor = result.data + table_at + (size_t)index * 8u;
        const uint32_t size = u32le(descriptor + 4);
        for (uint32_t step = 0; step < level; ++step) {
            mip_width = mip_width > 1u ? mip_width / 2u : 1u;
            mip_height = mip_height > 1u ? mip_height / 2u : 1u;
        }
        if (payload_at + size > result.size) goto done;
        stored.data = NULL;
        stored.size = 0;
        if (memcmp(descriptor, "COPY", 4) == 0) {
            stored.data = malloc(size == 0 ? 1u : size);
            if (stored.data == NULL) goto done;
            memcpy(stored.data, result.data + payload_at, size);
            stored.size = size;
        } else if (memcmp(descriptor, "LZ4 ", 4) != 0 ||
                !decode_lz4(result.data + payload_at, size, &stored)) {
            goto done;
        }
        if (stored.size != gpu_mip_bytes(format, mip_width, mip_height)) {
            free(stored.data);
            goto done;
        }
        decoded = malloc((size_t)mip_width * mip_height * 4u);
        if (decoded == NULL || !decode_gpu_mip(format, &stored, mip_width, mip_height, decoded)) {
            free(stored.data);
            goto done;
        }
        if (level == 0) {
            for (size_t pixel = 0; pixel < (size_t)width * height; ++pixel) {
                for (unsigned channel = 0; channel < 4; ++channel) {
                    int difference;
                    if (((format->channel_mask >> channel) & 1u) == 0) {
                        /* A channel the format does not carry has to read back as its own default. */
                        if (decoded[pixel * 4u + channel] != (channel == 3u ? 255u : 0u)) {
                            free(stored.data);
                            goto done;
                        }
                        continue;
                    }
                    difference = (int)decoded[pixel * 4u + channel] -
                        (int)expected[pixel * 4u + channel];
                    total += (uint64_t)(difference < 0 ? -difference : difference);
                    ++counted;
                }
            }
        }
        free(stored.data);
        free(decoded);
        decoded = NULL;
        payload_at += size;
    }
    ok = payload_at == result.size && counted != 0 && total <= (uint64_t)bound * counted;

done:
    if (!ok) {
        fprintf(stderr, "independent reader rejected %s as %s\n", result_path, format_name);
    }
    free(source.data);
    free(result.data);
    free(expected);
    free(decoded);
    return ok;
}

static int mip_equals(
    const bytes *file,
    uint32_t level,
    uint32_t width,
    uint32_t height,
    uint32_t mips,
    const uint8_t *expected,
    size_t expected_size
) {
    bytes decoded = { NULL, 0 };
    const int ok = selected_mip(file, level, width, height, mips, "COPY", &decoded) &&
        decoded.size == expected_size && memcmp(decoded.data, expected, expected_size) == 0;
    free(decoded.data);
    return ok;
}

/** Exact bytes for the controlled 3x2 TGA after each new mip-processing stage. */
static int verify_mip_modes(const char *kaiser_path, const char *pre_path, const char *post_path) {
    static const uint8_t source[] = {
        30, 20, 10, 255, 60, 50, 40, 255, 90, 80, 70, 255,
        120, 110, 100, 255, 150, 140, 130, 255, 180, 170, 160, 255
    };
    static const uint8_t kaiser_smallest[] = { 105, 95, 85, 255 };
    static const uint8_t pre_normalized[] = {
        61, 54, 47, 255, 64, 54, 45, 255, 70, 55, 40, 255,
        100, 61, 22, 255, 238, 190, 142, 255, 217, 200, 183, 255
    };
    static const uint8_t post_normalized_smallest[] = { 78, 56, 34, 255 };
    bytes kaiser = { NULL, 0 };
    bytes pre = { NULL, 0 };
    bytes post = { NULL, 0 };
    const int loaded = load(kaiser_path, &kaiser) && load(pre_path, &pre) && load(post_path, &post);
    const int ok = loaded &&
        mip_equals(&kaiser, 0, 3, 2, 2, source, sizeof source) &&
        mip_equals(&kaiser, 1, 3, 2, 2, kaiser_smallest, sizeof kaiser_smallest) &&
        mip_equals(&pre, 0, 3, 2, 1, pre_normalized, sizeof pre_normalized) &&
        mip_equals(&post, 0, 3, 2, 2, source, sizeof source) &&
        mip_equals(&post, 1, 3, 2, 2,
            post_normalized_smallest, sizeof post_normalized_smallest);
    free(kaiser.data);
    free(pre.data);
    free(post.data);
    if (!ok) fputs("independent reader rejected a mip-mode golden output\n", stderr);
    return ok;
}

int main(int argc, char **argv) {
    if (argc == 6 && strcmp(argv[1], "--gpu") == 0) {
        return verify_gpu(argv[2], argv[3], argv[4], (unsigned)strtoul(argv[5], NULL, 10)) ? 0 : 1;
    }
    if (argc == 5 && strcmp(argv[1], "--mips") == 0) {
        return verify_mip_modes(argv[2], argv[3], argv[4]) ? 0 : 1;
    }
    static const uint8_t copy_expected[] = { 30, 20, 10, 40 };
    static const uint8_t lz4_expected[] = { 3, 2, 1, 0, 6, 5, 4, 17 };
    bytes copy = { NULL, 0 };
    bytes lz4 = { NULL, 0 };
    bytes decoded = { NULL, 0 };
    bytes png = { NULL, 0 };
    bytes tga = { NULL, 0 };
    bytes jpg = { NULL, 0 };
    bytes tiff = { NULL, 0 };
    bytes dds = { NULL, 0 };
    int ok;
    if (argc != 3 && argc != 8) {
        fputs("usage: edds-reference-reader COPY LZ4 [PNG_RESULT TGA_RESULT JPG_RESULT TIFF_RESULT DDS_RESULT]\n"
            "       edds-reference-reader --mips KAISER PRE_NORMALIZE POST_NORMALIZE\n", stderr);
        return 2;
    }
    if (!load(argv[1], &copy) || !load(argv[2], &lz4)) {
        free(copy.data);
        free(lz4.data);
        return 1;
    }
    ok = selected_mip(&copy, 1, 3, 2, 2, "COPY", &decoded) &&
        decoded.size == sizeof copy_expected &&
        memcmp(decoded.data, copy_expected, sizeof copy_expected) == 0;
    free(decoded.data);
    decoded.data = NULL;
    decoded.size = 0;
    ok = ok && selected_mip(&lz4, 0, 2, 1, 1, "LZ4 ", &decoded) &&
        decoded.size == sizeof lz4_expected &&
        memcmp(decoded.data, lz4_expected, sizeof lz4_expected) == 0;
    free(decoded.data);
    decoded.data = NULL;
    decoded.size = 0;
    if (argc == 8) {
        static const uint8_t png_level_zero[] = {
            30, 20, 10, 40, 70, 60, 50, 80, 110, 100, 90, 120,
            130, 120, 110, 140, 170, 160, 150, 180, 210, 200, 190, 220
        };
        static const uint8_t png_level_one[] = { 120, 110, 100, 130 };
        static const uint8_t dds_level_zero[] = {
            33, 22, 11, 255, 66, 55, 44, 255
        };
        static const uint8_t dds_level_one[] = { 99, 88, 77, 255 };
        static const uint8_t three_by_two_level_zero[] = {
            30, 20, 10, 255, 60, 50, 40, 255, 90, 80, 70, 255,
            120, 110, 100, 255, 150, 140, 130, 255, 180, 170, 160, 255
        };
        ok = ok && load(argv[3], &png) && load(argv[4], &tga) &&
            u32le(png.data + 80) == 0x41u && u32le(tga.data + 80) == 0x40u &&
            selected_mip(&png, 0, 3, 2, 2, "COPY", &decoded) &&
            decoded.size == sizeof png_level_zero &&
            memcmp(decoded.data, png_level_zero, sizeof png_level_zero) == 0;
        free(decoded.data);
        decoded.data = NULL;
        decoded.size = 0;
        ok = ok && selected_mip(&png, 1, 3, 2, 2, "COPY", &decoded) &&
            decoded.size == sizeof png_level_one &&
            memcmp(decoded.data, png_level_one, sizeof png_level_one) == 0;
        free(decoded.data);
        decoded.data = NULL;
        decoded.size = 0;
        ok = ok && selected_mip(&tga, 0, 3, 2, 1, "COPY", &decoded) &&
            decoded.size == sizeof three_by_two_level_zero &&
            memcmp(decoded.data, three_by_two_level_zero, sizeof three_by_two_level_zero) == 0;
        free(decoded.data);
        decoded.data = NULL;
        decoded.size = 0;

        /*
         * The JPEG fixture is two flat DC-only halves, so its full level is grey 78 beside grey
         * 178 and the last mip of the box chain is their average. Both are judged within a bound.
         */
        {
            uint8_t jpeg_level_zero[16 * 8 * 4];
            static const uint8_t jpeg_smallest[] = { 128, 128, 128, 255 };
            for (size_t pixel = 0; pixel < 16u * 8u; ++pixel) {
                const uint8_t grey = pixel % 16u < 8u ? 78u : 178u;
                jpeg_level_zero[pixel * 4u] = grey;
                jpeg_level_zero[pixel * 4u + 1u] = grey;
                jpeg_level_zero[pixel * 4u + 2u] = grey;
                jpeg_level_zero[pixel * 4u + 3u] = 255u;
            }
            ok = ok && load(argv[5], &jpg) && load(argv[6], &tiff) &&
                u32le(jpg.data + 80) == 0x40u && u32le(tiff.data + 80) == 0x40u &&
                selected_mip(&jpg, 0, 16, 8, 5, "COPY", &decoded) &&
                within(&decoded, jpeg_level_zero, sizeof jpeg_level_zero, 2);
            free(decoded.data);
            decoded.data = NULL;
            decoded.size = 0;
            ok = ok && selected_mip(&jpg, 4, 16, 8, 5, "COPY", &decoded) &&
                within(&decoded, jpeg_smallest, sizeof jpeg_smallest, 2);
            free(decoded.data);
            decoded.data = NULL;
            decoded.size = 0;
        }
        ok = ok && selected_mip(&tiff, 0, 3, 2, 1, "COPY", &decoded) &&
            decoded.size == sizeof three_by_two_level_zero &&
            memcmp(decoded.data, three_by_two_level_zero, sizeof three_by_two_level_zero) == 0;
        free(decoded.data);
        decoded.data = NULL;
        decoded.size = 0;
        ok = ok && load(argv[7], &dds) &&
            selected_mip(&dds, 0, 2, 1, 2, "COPY", &decoded) &&
            decoded.size == sizeof dds_level_zero &&
            memcmp(decoded.data, dds_level_zero, sizeof dds_level_zero) == 0;
        free(decoded.data);
        decoded.data = NULL;
        decoded.size = 0;
        ok = ok && selected_mip(&dds, 1, 2, 1, 2, "COPY", &decoded) &&
            decoded.size == sizeof dds_level_one &&
            memcmp(decoded.data, dds_level_one, sizeof dds_level_one) == 0;
        free(decoded.data);
    }
    free(copy.data);
    free(lz4.data);
    free(png.data);
    free(tga.data);
    free(jpg.data);
    free(tiff.data);
    free(dds.data);
    if (!ok) {
        fputs("independent EDDS reference reader disagreed with the fixture\n", stderr);
    }
    return ok ? 0 : 1;
}
