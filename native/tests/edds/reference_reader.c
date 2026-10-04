/*
 * edds-reference-reader: an independent reader of the converter's output that shares no code with
 * it. It reads an EDDS itself (the header, the mip table, COPY and LZ4 mips, the GPU block formats)
 * and compares what it finds with the pixels the tests expect. It exits 0 when everything matches,
 * 1 when something does not, and 2 when the arguments do not fit.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** A file read into memory, or a mip read out of one: the bytes and how many there are. */
typedef struct bytes {
    uint8_t *data;
    size_t   size;
} bytes;

/** Reads a 32-bit number stored low byte first. */
static uint32_t u32le(const uint8_t *at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

/**
 * Reads the whole file at `path` into `result`. Returns 1 on success, the caller then owning
 * result->data; 0 on failure, with result->data NULL.
 */
static int load(const char *path, bytes *result) {
    FILE *file = fopen(path, "rb");
    long  length;
    int   read_ok;
    int   close_ok;

    result->data = NULL;
    result->size = 0;

    /* The file's length, from the end it seeks to, then back to its start. */
    if (file == NULL || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
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

    read_ok  = fread(result->data, 1, result->size, file) == result->size;
    close_ok = fclose(file) == 0;

    if (!read_ok || !close_ok) {
        free(result->data);
        result->data = NULL;
        result->size = 0;
        return 0;
    }

    return 1;
}

/**
 * Reads one LZ4 length into `value`, starting from `base`, the four bits a token gave it. A base
 * of 15 goes on in the bytes at `*at`: each is added, and one below 255 is the last. Returns 0
 * when the input runs out first or the sum overflows.
 */
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

/**
 * Decodes one mip stored LZ4: its decoded size, then blocks, each after 4 bytes that hold its
 * compressed size in the low 31 bits and, in the top bit, whether it is the last. A block is a run
 * of sequences: literals copied as they are, then a match copied from the output so far. Returns
 * 1 when the payload decodes to exactly the declared size, the caller then owning decoded->data;
 * 0 otherwise.
 */
static int decode_lz4(const uint8_t *payload, size_t stored, bytes *decoded) {
    size_t input_at  = 4;
    size_t output_at = 0;
    int    saw_final = 0;

    if (stored < 8u) {
        return 0;
    }

    /* The decoded size comes first, and the output gets exactly that much room. */
    decoded->size = u32le(payload);
    decoded->data = malloc(decoded->size == 0 ? 1u : decoded->size);

    if (decoded->size == 0 || decoded->data == NULL) {
        return 0;
    }

    /* The blocks, one by one, until the one marked last. */
    while (input_at < stored && !saw_final) {
        uint32_t framed;
        size_t   block_end;

        if (stored - input_at < 4u) {
            goto failure;
        }

        framed    = u32le(payload + input_at);
        input_at += 4u;

        if ((size_t)(framed & 0x7fffffffu) > stored - input_at) {
            goto failure;
        }

        block_end = input_at + (framed & 0x7fffffffu);

        /* The sequences of the block, each a token, its literals and then its match. */
        while (input_at < block_end) {
            const uint8_t token = payload[input_at++];
            size_t        literals;
            size_t        match;
            uint32_t      offset;

            /* The literal count from the token's high four bits, then the literals themselves. */
            if (!length(payload, block_end, &input_at, token >> 4, &literals) ||
                literals > block_end - input_at ||
                literals > decoded->size - output_at) {
                goto failure;
            }

            memcpy(decoded->data + output_at, payload + input_at, literals);
            input_at  += literals;
            output_at += literals;

            /* The sequence that ends its block has no match. */
            if (input_at == block_end) {
                break;
            }

            if (block_end - input_at < 2u) {
                goto failure;
            }

            /*
             * The match: how far back it starts, 2 bytes low first, and its length from the
             * token's low four bits plus 4. It starts inside the output so far and fits the rest.
             */
            offset    = (uint32_t)payload[input_at] | ((uint32_t)payload[input_at + 1u] << 8);
            input_at += 2u;

            if (offset == 0 ||
                offset > output_at ||
                !length(payload, block_end, &input_at, token & 0x0fu, &match) ||
                match > SIZE_MAX - 4u) {
                goto failure;
            }

            match += 4u;

            if (match > decoded->size - output_at) {
                goto failure;
            }

            /* Byte by byte, so a match can repeat bytes it has just written. */
            for (size_t index = 0; index < match; ++index) {
                decoded->data[output_at] = decoded->data[output_at - offset];
                ++output_at;
            }
        }

        saw_final = (framed & 0x80000000u) != 0;
    }

    /* The last block must have come, and the payload and the output must both be used up. */
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

/**
 * Reads mip `level` (0 the largest) of an EDDS whose header gives the expected size and mip count,
 * and whose table entry for that mip names the expected container, COPY or "LZ4 ". Every entry of
 * the table is checked, and the stored mips must end exactly where the file does. Returns 1 with
 * the mip's bytes in `decoded`, which the caller then owns; 0 when anything does not match.
 */
static int selected_mip(
    const bytes *file,
    uint32_t     level,
    uint32_t     expected_width,
    uint32_t     expected_height,
    uint32_t     expected_mips,
    const char   expected_container[4],
    bytes       *decoded) {
    /* The mip count, and the wanted mip's place in the table, which lists the smallest first. */
    uint32_t mips;
    uint32_t stored_index;

    /* Where the table starts, where the wanted mip's bytes start, how far the mips reach so far. */
    size_t table_at         = 128u;
    size_t selected_payload = 0;
    size_t complete;

    /* The wanted mip's stored size, and its entry in the table. */
    uint32_t       selected_size       = 0;
    const uint8_t *selected_descriptor = NULL;

    decoded->data = NULL;
    decoded->size = 0;

    /* The DDS header with the ENF1 marker, the expected size, and room for one table entry. */
    if (file->size < 136u ||
        memcmp(file->data, "DDS ", 4) != 0 ||
        u32le(file->data + 4) != 124u ||
        memcmp(file->data + 36, "ENF1", 4) != 0 ||
        u32le(file->data + 16) != expected_width ||
        u32le(file->data + 12) != expected_height) {
        return 0;
    }

    /* A DX10 header moves the table 20 bytes on. */
    if (memcmp(file->data + 84, "DX10", 4) == 0) {
        table_at = 148u;
    }

    if (file->size < table_at) {
        return 0;
    }

    /* The mip count, 0 meaning 1: the expected one, at most 32, and a table that fits the file. */
    mips = u32le(file->data + 28);

    if (mips == 0) {
        mips = 1;
    }

    if (mips != expected_mips || level >= mips || mips > 32u || (size_t)mips > (file->size - table_at) / 8u) {
        return 0;
    }

    complete     = table_at + (size_t)mips * 8u;
    stored_index = mips - level - 1u;

    /*
     * The table, entry by entry: COPY or LZ4, with a stored size that is not 0 and fits the file.
     * The wanted mip's entry, and where its bytes start, are kept on the way.
     */
    for (uint32_t index = 0; index < mips; ++index) {
        const uint8_t *descriptor = file->data + table_at + (size_t)index * 8u;
        const uint32_t stored     = u32le(descriptor + 4);

        if ((memcmp(descriptor, "COPY", 4) != 0 && memcmp(descriptor, "LZ4 ", 4) != 0) || stored == 0 || stored > file->size - complete) {
            return 0;
        }

        if (index == stored_index) {
            selected_descriptor = descriptor;
            selected_size       = stored;
            selected_payload    = complete;
        }

        complete += stored;
    }

    /* The mips end where the file ends, and the wanted one is in the expected container. */
    if (complete != file->size || selected_descriptor == NULL || memcmp(selected_descriptor, expected_container, 4) != 0) {
        return 0;
    }

    /* A COPY mip is its bytes as they are; an LZ4 one is decoded. */
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
 * this reader checks is lossless and is compared exactly. Returns 1 when `decoded` holds `size`
 * bytes and none differs from `expected` by more than `tolerance`.
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

/** One GPU format as this reader knows it: how the header names it and how its mips are stored. */
typedef struct gpu_format {
    /** The name the command line gives it. */
    const char *name;
    /** The FourCC in the header: empty for BGRA8 and BGRX8, DX10 for a format named by DXGI. */
    const char *four_cc;
    /** The DXGI format in the DX10 header, 0 without one. */
    uint32_t    dxgi;
    /** The bytes of a 4x4 block, 0 for a format stored pixel by pixel. */
    uint32_t    block_bytes;
    /** The bytes of a pixel, 0 for a block format. */
    uint32_t    pixel_bytes;
    /** Which of R, G, B, A the format really carries, as a mask over the channel index. */
    unsigned    channel_mask;
} gpu_format;

/** The formats this reader can check, one row each, in the order of the fields above. */
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

/** The format called `name`, or NULL when this reader does not know it. */
static const gpu_format *gpu_format_named(const char *name) {
    for (size_t at = 0; at < sizeof gpu_formats / sizeof gpu_formats[0]; ++at) {
        if (strcmp(gpu_formats[at].name, name) == 0) {
            return &gpu_formats[at];
        }
    }

    return NULL;
}

/** How many 4-pixel blocks cover `value` pixels, a part block counting as a whole one. */
static uint32_t blocks_across(uint32_t value) {
    return (value + 3u) / 4u;
}

/**
 * The bytes a `width` by `height` mip takes in `format`: whole blocks for a block format, so many
 * bytes a pixel otherwise.
 */
static uint32_t gpu_mip_bytes(const gpu_format *format, uint32_t width, uint32_t height) {
    return format->block_bytes != 0
        ? blocks_across(width) * blocks_across(height) * format->block_bytes
        : width * height * format->pixel_bytes;
}

/**
 * One channel of a 16-bit 5:6:5 colour, `bits` wide from bit `shift`, widened to 8 bits with its
 * high bits repeated below it.
 */
static uint8_t channel_of_565(uint32_t value, unsigned shift, unsigned bits) {
    const uint32_t part = (value >> shift) & ((1u << bits) - 1u);

    return (uint8_t)((part << (8u - bits)) | (part >> (2u * bits - 8u)));
}

/** The weighted average of `low` and `high`, their weights out of `total`, rounded to nearest. */
static uint8_t blend(uint32_t low, uint32_t high, uint32_t low_parts, uint32_t high_parts, uint32_t total) {
    return (uint8_t)((low * low_parts + high * high_parts + total / 2u) / total);
}

/**
 * Decodes one 8-byte BC1 block into the 16 RGBA pixels of its 4x4 square, row by row: two 5:6:5
 * endpoints, the colours between them, and a 2-bit index a pixel that picks one of four.
 * Standing alone the endpoint order picks the layout; inside BC3 it is always four colours.
 */
static void decode_bc1(const uint8_t block[8], int always_four_colours, uint8_t rgba[64]) {
    const uint32_t first        = (uint32_t)block[0] | ((uint32_t)block[1] << 8);
    const uint32_t second       = (uint32_t)block[2] | ((uint32_t)block[3] << 8);
    const int      four_colours = always_four_colours || first > second;
    uint8_t        palette[4][4];

    /* The endpoints are the first two colours, opaque. */
    for (unsigned entry = 0; entry < 2; ++entry) {
        const uint32_t value = entry == 0 ? first : second;

        palette[entry][0] = channel_of_565(value, 11, 5);
        palette[entry][1] = channel_of_565(value, 5, 6);
        palette[entry][2] = channel_of_565(value, 0, 5);
        palette[entry][3] = 255u;
    }

    /*
     * The other two: a third and two thirds of the way from the first endpoint to the second, or,
     * in the three-colour layout, halfway and transparent black.
     */
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

    /* Each pixel's 2-bit index, four to a byte from the lowest bits up, picks its colour. */
    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        const unsigned index = (block[4u + pixel / 4u] >> ((pixel % 4u) * 2u)) & 3u;

        memcpy(rgba + pixel * 4u, palette[index], 4);
    }
}

/**
 * Decodes one 8-byte BC4 block into the 16 values of its 4x4 square, row by row: two endpoints,
 * the values between them, and a 3-bit index a pixel that picks one of eight.
 */
static void decode_bc4(const uint8_t block[8], uint8_t values[16]) {
    uint8_t  palette[8];
    uint64_t packed = 0;

    palette[0] = block[0];
    palette[1] = block[1];

    /* A larger first endpoint gives six values between the two; otherwise four, then 0 and 255. */
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

    /* The 48 bits of indices, three a pixel, the first pixel's in the lowest bits. */
    for (unsigned byte = 0; byte < 6; ++byte) {
        packed |= (uint64_t)block[2u + byte] << (byte * 8u);
    }

    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        values[pixel] = palette[(packed >> (pixel * 3u)) & 7u];
    }
}

/**
 * The two partition tables this reader needs, written out from the block layout on their own.
 * This one has a row for each of the 64 partitions: for each of the 16 pixels, row by row, the
 * subset it belongs to.
 */
static const uint8_t reference_partitions[64][16] = {
    { 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1 },
    { 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1 },
    { 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1 },
    { 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1 },
    { 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1 },
    { 0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1 },
    { 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1 },
    { 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1 },
    { 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1 },
    { 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0 },
    { 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0 },
    { 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0 },
    { 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1 },
    { 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0 },
    { 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0 },
    { 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0 },
    { 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0 },
    { 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0 },
    { 0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0 },
    { 0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0 },
    { 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1 },
    { 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1 },
    { 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0 },
    { 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0 },
    { 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0 },
    { 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0 },
    { 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1 },
    { 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1 },
    { 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0 },
    { 0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0 },
    { 0, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0 },
    { 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0 },
    { 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0 },
    { 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1 },
    { 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1 },
    { 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0 },
    { 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0 },
    { 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0 },
    { 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1 },
    { 0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1 },
    { 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0 },
    { 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0 },
    { 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1 },
    { 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1 },
    { 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1 },
    { 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1 },
    { 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1 },
    { 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0 },
    { 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0 },
    { 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1 }
};

/**
 * For each partition, the pixel that anchors its second subset: its index, like pixel 0's, is
 * stored one bit shorter.
 */
static const uint8_t reference_anchor[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
    6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15
};

/** Reads `count` bits of the block from bit `*at`, the lowest first, and moves `*at` past them. */
static uint32_t reference_bits(const uint8_t *block, unsigned *at, unsigned count) {
    uint32_t value = 0;

    for (unsigned bit = 0; bit < count; ++bit) {
        const unsigned position = *at + bit;

        value |= (uint32_t)((block[position >> 3] >> (position & 7u)) & 1u) << bit;
    }

    *at += count;

    return value;
}

/**
 * Widens a value `bits` wide to 8 bits with its high bits repeated below it; 8 bits or more pass
 * as they are.
 */
static uint8_t reference_expand(uint32_t value, unsigned bits) {
    return bits >= 8 ? (uint8_t)value : (uint8_t)((value << (8u - bits)) | (value >> (2u * bits - 8u)));
}

/**
 * Decodes the three BC7 modes this converter writes. A block in any other mode is refused rather
 * than guessed at: an oracle that accepted a block it could not read would prove nothing. Returns
 * 1 with the 16 RGBA pixels of the 4x4 square in `rgba`, row by row; 0 for a refused block.
 */
static int decode_bc7(const uint8_t block[16], uint8_t rgba[64]) {
    /* The weights, out of 64, that 2-, 3- and 4-bit indices pick between two endpoints. */
    static const uint8_t weights_two[4]   = { 0, 21, 43, 64 };
    static const uint8_t weights_three[8] = { 0, 9, 18, 27, 37, 46, 55, 64 };
    static const uint8_t weights_four[16] = {
        0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 56, 60, 64
    };

    /* The next bit to read, and the block's mode. */
    unsigned at   = 0;
    unsigned mode = 0;

    /* What the mode sets: subsets, channels, bits per endpoint channel, bits per index. */
    unsigned subsets;
    unsigned channels;
    unsigned bits;
    unsigned index_bits;

    /* The partition's anchor and number, the weights the mode uses, and each pixel's subset. */
    unsigned       anchor    = 0;
    uint32_t       partition = 0;
    const uint8_t *weights;
    const uint8_t *membership;

    /* The endpoints widened to 8 bits, each pixel's index, the endpoints as stored, the P-bits. */
    uint8_t  endpoints[4][4];
    uint8_t  indices[16];
    uint32_t raw[4][4] = { { 0 } };
    uint32_t p[4]      = { 0, 0, 0, 0 };

    /* The mode is the number of 0 bits before the first 1. */
    while (mode < 8 && reference_bits(block, &at, 1) == 0) {
        ++mode;
    }

    /* The three modes this reader takes, and what each stores. */
    if (mode == 1) {
        subsets    = 2;
        channels   = 3;
        bits       = 6;
        index_bits = 3;
        weights    = weights_three;
    } else if (mode == 6) {
        subsets    = 1;
        channels   = 4;
        bits       = 7;
        index_bits = 4;
        weights    = weights_four;
    } else if (mode == 7) {
        subsets    = 2;
        channels   = 4;
        bits       = 5;
        index_bits = 2;
        weights    = weights_two;
    } else {
        return 0;
    }

    /* Two subsets: a 6-bit partition number, which says the subset of each pixel and the anchor. */
    if (subsets == 2) {
        partition = reference_bits(block, &at, 6);
        anchor    = reference_anchor[partition];
    }

    membership = subsets == 2 ? reference_partitions[partition] : NULL;

    /* The endpoints channel by channel: every endpoint's red, then every green, and so on. */
    for (unsigned channel = 0; channel < channels; ++channel) {
        for (unsigned endpoint = 0; endpoint < subsets * 2u; ++endpoint) {
            raw[endpoint][channel] = reference_bits(block, &at, bits);
        }
    }

    /* Mode 1 shares one P-bit across each subset; modes 6 and 7 give every endpoint its own. */
    if (mode == 1) {
        for (unsigned subset = 0; subset < 2; ++subset) {
            const uint32_t shared = reference_bits(block, &at, 1);

            p[subset * 2u]      = shared;
            p[subset * 2u + 1u] = shared;
        }
    } else {
        for (unsigned endpoint = 0; endpoint < subsets * 2u; ++endpoint) {
            p[endpoint] = reference_bits(block, &at, 1);
        }
    }

    /*
     * Each endpoint channel with its P-bit appended as the lowest bit, widened to 8 bits; a channel
     * the mode does not store is 255.
     */
    for (unsigned endpoint = 0; endpoint < subsets * 2u; ++endpoint) {
        for (unsigned channel = 0; channel < 4; ++channel) {
            endpoints[endpoint][channel] = channel >= channels
                ? 255u
                : reference_expand((raw[endpoint][channel] << 1) | p[endpoint], bits + 1u);
        }
    }

    /* The indices: pixel 0's, and the second subset's anchor's, are a bit shorter than the rest. */
    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        const unsigned count = index_bits - (pixel == 0 || (subsets == 2 && pixel == anchor) ? 1u : 0u);

        indices[pixel] = (uint8_t)reference_bits(block, &at, count);
    }

    /* A block whose fields do not take exactly its 128 bits is refused. */
    if (at != 128u) {
        return 0;
    }

    /*
     * Each pixel between the two endpoints of its subset: the weight its index picks, out of 64,
     * goes to the second endpoint and the rest to the first, rounded to nearest.
     */
    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        const unsigned subset = membership == NULL ? 0u : membership[pixel];

        for (unsigned channel = 0; channel < 4; ++channel) {
            const uint32_t low  = endpoints[subset * 2u][channel];
            const uint32_t high = endpoints[subset * 2u + 1u][channel];

            rgba[pixel * 4u + channel] = (uint8_t)((low * (64u - weights[indices[pixel]]) + high * weights[indices[pixel]] + 32u) >> 6);
        }
    }

    return 1;
}

/**
 * Copies a decoded 4x4 block into the `width` by `height` RGBA image with its top-left corner at
 * (`left`, `top`), leaving out the pixels that fall past the image's right or bottom edge.
 */
static void place_block(
    const uint8_t decoded[64],
    uint32_t      width,
    uint32_t      height,
    uint32_t      left,
    uint32_t      top,
    uint8_t      *rgba) {
    for (unsigned row = 0; row < 4 && top + row < height; ++row) {
        for (unsigned column = 0; column < 4 && left + column < width; ++column) {
            memcpy(rgba + (((size_t)(top + row) * width) + left + column) * 4u, decoded + (row * 4u + column) * 4u, 4);
        }
    }
}

/**
 * Decodes one stored mip of `format`, `width` by `height`, into RGBA. A channel the format does
 * not carry reads as 0, and alpha as 255. Returns 0 when the stored size is not the one the size
 * needs or a BC7 block is refused; 1 otherwise.
 */
static int decode_gpu_mip(
    const gpu_format *format,
    const bytes      *stored,
    uint32_t          width,
    uint32_t          height,
    uint8_t          *rgba) {
    if (stored->size != gpu_mip_bytes(format, width, height)) {
        return 0;
    }

    /* Pixel by pixel: BGRA or BGRX in four bytes, red and green in two, red alone in one. */
    if (format->block_bytes == 0) {
        for (size_t pixel = 0; pixel < (size_t)width * height; ++pixel) {
            const uint8_t *at  = stored->data + pixel * format->pixel_bytes;
            uint8_t       *out = rgba + pixel * 4u;

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

    /* A block format: each 4x4 block decoded, then placed in the image. */
    for (uint32_t row = 0; row < blocks_across(height); ++row) {
        for (uint32_t column = 0; column < blocks_across(width); ++column) {
            const uint8_t *block = stored->data + ((size_t)row * blocks_across(width) + column) * format->block_bytes;
            uint8_t        decoded[64];
            uint8_t        channel[16];
            uint8_t        second[16];

            memset(decoded, 0, sizeof decoded);

            /*
             * DXT1 is one BC1 block. DXT5 is a BC4 block of alpha, then a BC1 block of colour that
             * always has four colours. BC4 is a block of red, BC5 a block of red and one of green,
             * both opaque. Anything else is BC7.
             */
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
                    decoded[pixel * 4u]      = channel[pixel];
                    decoded[pixel * 4u + 3u] = 255u;
                }
            } else if (strcmp(format->name, "BC5") == 0) {
                decode_bc4(block, channel);
                decode_bc4(block + 8, second);

                for (unsigned pixel = 0; pixel < 16; ++pixel) {
                    decoded[pixel * 4u]      = channel[pixel];
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

/**
 * The source image, read back out of the TGA the fixture wrote, as straight RGBA. Returns 1
 * with its size in `*width` and `*height` and a new buffer in `*rgba`, which the caller frees; 0
 * when the TGA is not an uncompressed 24- or 32-bit one of exactly the size its header gives.
 */
static int expected_source(const bytes *tga, uint32_t *width, uint32_t *height, uint8_t **rgba) {
    size_t pixels;
    size_t stride;

    /* An uncompressed true-colour TGA (type 2), 24 or 32 bits a pixel. */
    if (tga->size < 18u || tga->data[2] != 2u || (tga->data[16] != 32u && tga->data[16] != 24u)) {
        return 0;
    }

    /* The size from the header, low byte first, and exactly that many pixels after it. */
    stride  = tga->data[16] == 32u ? 4u : 3u;
    *width  = (uint32_t)tga->data[12] | ((uint32_t)tga->data[13] << 8);
    *height = (uint32_t)tga->data[14] | ((uint32_t)tga->data[15] << 8);
    pixels  = (size_t)*width * *height;

    if (tga->size != 18u + pixels * stride) {
        return 0;
    }

    *rgba = malloc(pixels * 4u);

    if (*rgba == NULL) {
        return 0;
    }

    /* Each pixel from blue first to red first. */
    for (size_t pixel = 0; pixel < pixels; ++pixel) {
        const uint8_t *at = tga->data + 18u + pixel * stride;

        (*rgba)[pixel * 4u]      = at[2];
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
 * the source within the bound, and the smallest mip of the chain decodes at all. Returns 1 when
 * all of that holds; otherwise it prints which file it rejected and returns 0.
 */
static int verify_gpu(const char *source_path, const char *result_path, const char *format_name, unsigned bound) {
    /* The source TGA, the converted file, and one mip as it is stored in it. */
    bytes source = { NULL, 0 };
    bytes result = { NULL, 0 };
    bytes stored = { NULL, 0 };

    /* The format, the source as RGBA, and one mip decoded to RGBA. */
    const gpu_format *format   = gpu_format_named(format_name);
    uint8_t          *expected = NULL;
    uint8_t          *decoded  = NULL;

    /* The source's size, the mip count, the header's size, and where the table and mips start. */
    uint32_t width  = 0;
    uint32_t height = 0;
    uint32_t mips;
    uint32_t header_bytes;
    size_t   table_at;
    size_t   payload_at;

    /* The difference summed over the top mip, how many values it covers, and the verdict. */
    uint64_t total   = 0;
    unsigned counted = 0;
    int      ok      = 0;

    if (format == NULL ||
        !load(source_path, &source) ||
        !load(result_path, &result) ||
        !expected_source(&source, &width, &height, &expected)) {
        goto done;
    }

    /* The DDS header with the ENF1 marker, and the source's size. */
    if (result.size < 128u ||
        memcmp(result.data, "DDS ", 4) != 0 ||
        memcmp(result.data + 36, "ENF1", 4) != 0 ||
        u32le(result.data + 16) != width ||
        u32le(result.data + 12) != height) {
        goto done;
    }

    mips         = u32le(result.data + 28);
    header_bytes = 128u;

    /*
     * The format as the header names it: BGRA8 and BGRX8 without the FourCC flag (0x4), the others
     * with it and their FourCC, and a DXGI format with a DX10 header too, which holds the format,
     * a 2D texture (3) and an array of one, and moves the table 20 bytes on.
     */
    if (format->four_cc[0] == '\0') {
        if ((u32le(result.data + 80) & 0x4u) != 0) {
            goto done;
        }
    } else {
        if ((u32le(result.data + 80) & 0x4u) == 0 || memcmp(result.data + 84, format->four_cc, 4) != 0) {
            goto done;
        }

        if (format->dxgi != 0) {
            if (result.size < 148u ||
                u32le(result.data + 128) != format->dxgi ||
                u32le(result.data + 132) != 3u ||
                u32le(result.data + 140) != 1u) {
                goto done;
            }

            header_bytes = 148u;
        }
    }

    /* A block format declares its top mip as a linear size; an uncompressed one as a pitch. */
    if (u32le(result.data + 20) != (format->block_bytes != 0 ? gpu_mip_bytes(format, width, height) : width * format->pixel_bytes)) {
        goto done;
    }

    /* The flags agree: 0x80000, a linear size, for a block format; 0x8, a pitch, otherwise. */
    if ((u32le(result.data + 8) & 0x80008u) != (format->block_bytes != 0 ? 0x80000u : 0x8u)) {
        goto done;
    }

    /* The mips in the order of the table, the smallest first, each read and decoded. */
    table_at   = header_bytes;
    payload_at = table_at + (size_t)mips * 8u;

    for (uint32_t index = 0; index < mips; ++index) {
        const uint32_t level      = mips - index - 1u;
        uint32_t       mip_width  = width;
        uint32_t       mip_height = height;
        const uint8_t *descriptor = result.data + table_at + (size_t)index * 8u;
        const uint32_t size       = u32le(descriptor + 4);

        /* The mip's size: halved for each level down, but never below 1. */
        for (uint32_t step = 0; step < level; ++step) {
            mip_width  = mip_width > 1u ? mip_width / 2u : 1u;
            mip_height = mip_height > 1u ? mip_height / 2u : 1u;
        }

        if (payload_at + size > result.size) {
            goto done;
        }

        /* The stored mip, copied for COPY, decoded for LZ4; any other container is rejected. */
        stored.data = NULL;
        stored.size = 0;

        if (memcmp(descriptor, "COPY", 4) == 0) {
            stored.data = malloc(size == 0 ? 1u : size);

            if (stored.data == NULL) {
                goto done;
            }

            memcpy(stored.data, result.data + payload_at, size);
            stored.size = size;
        } else if (memcmp(descriptor, "LZ4 ", 4) != 0 || !decode_lz4(result.data + payload_at, size, &stored)) {
            goto done;
        }

        /* Exactly the bytes the mip's size needs in the format, then decoded to RGBA. */
        if (stored.size != gpu_mip_bytes(format, mip_width, mip_height)) {
            free(stored.data);
            goto done;
        }

        decoded = malloc((size_t)mip_width * mip_height * 4u);

        if (decoded == NULL || !decode_gpu_mip(format, &stored, mip_width, mip_height, decoded)) {
            free(stored.data);
            goto done;
        }

        /* The top mip against the source, channel by channel. */
        if (level == 0) {
            for (size_t pixel = 0; pixel < (size_t)width * height; ++pixel) {
                for (unsigned channel = 0; channel < 4; ++channel) {
                    int difference;

                    /* A channel the format does not carry has to read back as its own default. */
                    if (((format->channel_mask >> channel) & 1u) == 0) {
                        if (decoded[pixel * 4u + channel] != (channel == 3u ? 255u : 0u)) {
                            free(stored.data);
                            goto done;
                        }

                        continue;
                    }

                    difference  = (int)decoded[pixel * 4u + channel] - (int)expected[pixel * 4u + channel];
                    total      += (uint64_t)(difference < 0 ? -difference : difference);
                    ++counted;
                }
            }
        }

        free(stored.data);
        free(decoded);
        decoded     = NULL;
        payload_at += size;
    }

    /* The mips end where the file ends, and the mean difference stays within the bound. */
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

/**
 * Whether mip `level` of an EDDS of the given size and mip count is stored COPY and holds exactly
 * the `expected` bytes. Returns 1 when it does, 0 otherwise.
 */
static int mip_equals(
    const bytes   *file,
    uint32_t       level,
    uint32_t       width,
    uint32_t       height,
    uint32_t       mips,
    const uint8_t *expected,
    size_t         expected_size) {
    bytes     decoded = { NULL, 0 };
    const int ok      = selected_mip(file, level, width, height, mips, "COPY", &decoded) &&
        decoded.size == expected_size &&
        memcmp(decoded.data, expected, expected_size) == 0;

    free(decoded.data);
    return ok;
}

/**
 * Exact bytes for the controlled 3x2 TGA after each new mip-processing stage. The Kaiser file
 * keeps the source as its top mip and adds a 1x1 one; the pre file is a single normalized mip;
 * the post file keeps the source on top and adds a normalized 1x1. Returns 1 when all three
 * match; otherwise it prints a line and returns 0.
 */
static int verify_mip_modes(const char *kaiser_path, const char *pre_path, const char *post_path) {
    /* The source's pixels, and the mips each stage must add or change. */
    static const uint8_t source[] = {
        30, 20, 10, 255, 60, 50, 40, 255, 90, 80, 70, 255,
        120, 110, 100, 255, 150, 140, 130, 255, 180, 170, 160, 255
    };
    static const uint8_t kaiser_smallest[] = { 105, 95, 85, 255 };
    static const uint8_t pre_normalized[]  = {
        61, 54, 47, 255, 64, 54, 45, 255, 70, 55, 39, 255,
        99, 61, 22, 255, 240, 190, 140, 255, 218, 201, 184, 255
    };
    static const uint8_t post_normalized_smallest[] = { 78, 56, 34, 255 };

    /* The three files, and whether all of them could be read. */
    bytes     kaiser = { NULL, 0 };
    bytes     pre    = { NULL, 0 };
    bytes     post   = { NULL, 0 };
    const int loaded = load(kaiser_path, &kaiser) && load(pre_path, &pre) && load(post_path, &post);

    /* Every mip of the three files against what it must hold. */
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

    if (!ok) {
        fputs("independent reader rejected a mip-mode golden output\n", stderr);
    }

    return ok;
}

/**
 * Checks an EDDS converted from a Workbench capture against the capture. `hex` holds the bytes of
 * all `count` levels of the full chain, from `width` by `height` down, two hex digits a byte. The
 * file must hold the levels left after the `removed` largest ones, each stored COPY with exactly
 * those bytes. Returns 1 when it does; otherwise it prints the path and returns 0.
 */
static int verify_golden(const char *path, unsigned width, unsigned height, unsigned count, const char *hex, unsigned removed) {
    bytes    file      = { NULL, 0 };
    int      ok        = load(path, &file);
    unsigned top_width = width, top_height = height;
    size_t   cursor = 0;

    /* Level by level down the full chain, its expected bytes read from the hex as it goes. */
    for (unsigned level = 0; level < count; ++level) {
        uint8_t      expected[32 * 32 * 4];
        const size_t size = (size_t)width * height * 4;

        if (size > sizeof expected || strlen(hex + cursor) < size * 2) {
            ok = 0;
            break;
        }

        for (size_t at = 0; at < size; ++at) {
            char byte[3] = { hex[cursor], hex[cursor + 1], 0 };

            expected[at]  = (uint8_t)strtoul(byte, NULL, 16);
            cursor       += 2;
        }

        /*
         * A removed level only moves the top of the stored chain down a level; any other must be
         * in the file, counted from that top.
         */
        if (level < removed) {
            top_width  = width > 1 ? width / 2 : 1;
            top_height = height > 1 ? height / 2 : 1;
        } else {
            ok = ok && mip_equals(&file, level - removed, top_width, top_height, count - removed, expected, size);
        }

        /* The next level's size: halved, but never below 1. */
        width  = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }

    /* The hex holds the chain and nothing more. */
    ok = ok && hex[cursor] == 0;
    free(file.data);

    if (!ok) {
        fprintf(stderr, "Workbench golden mismatch: %s\n", path);
    }

    return ok;
}

/**
 * Checks an EDDS converted with a swizzle against Workbench's capture of the same conversion. The
 * header must name the format called `format_name`; then each of the `count` mips, stored COPY, is
 * decoded and compared channel by channel with the BGRA bytes in `hex`, two hex digits a byte. An
 * uncompressed format must match exactly; a block format may be off by a mean of 6 a pixel in
 * alpha and 8 in a colour channel. Returns 1 when everything matches; otherwise it prints what
 * failed and returns 0.
 */
static int verify_swizzle(const char *path, unsigned width, unsigned height, unsigned count, const char *hex, const char *format_name) {
    const gpu_format *format = gpu_format_named(format_name);
    bytes             file   = { NULL, 0 };
    int               ok     = format != NULL && load(path, &file) && file.size >= 136u;

    /*
     * The header names the format: by its FourCC, and for a DXGI format in the DX10 header too; or
     * else as 32-bit RGB, with the alpha flag (0x41 rather than 0x40) when the format has alpha.
     */
    if (ok && format->four_cc[0] != '\0') {
        ok = memcmp(file.data + 84, format->four_cc, 4) == 0;

        if (ok && format->dxgi != 0) {
            ok = file.size >= 148u && u32le(file.data + 128) == format->dxgi;
        }
    } else if (ok) {
        ok = u32le(file.data + 80) == (format->channel_mask == 0xfu ? 0x41u : 0x40u) && u32le(file.data + 88) == 32u;
    }

    const unsigned top_width = width, top_height = height;
    size_t         cursor = 0;

    /* Mip by mip, as long as everything has matched. */
    for (unsigned level = 0; ok && level < count; ++level) {
        uint8_t      expected[32 * 32 * 4], decoded[sizeof expected];
        const size_t size   = (size_t)width * height * 4u;
        bytes        stored = { NULL, 0 };

        if (size > sizeof expected || strlen(hex + cursor) < size * 2u) {
            ok = 0;
            break;
        }

        /* The mip's expected bytes, from the hex. */
        for (size_t at = 0; at < size; ++at) {
            char byte[3] = { hex[cursor], hex[cursor + 1], 0 };

            expected[at]  = (uint8_t)strtoul(byte, NULL, 16);
            cursor       += 2;
        }

        ok = selected_mip(&file, level, top_width, top_height, count, "COPY", &stored) &&
            decode_gpu_mip(format, &stored, width, height, decoded);

        /*
         * Channel by channel in RGBA order, against the expected bytes in BGRA order: the
         * difference summed over the mip's pixels must stay within the channel's bound.
         */
        for (unsigned c = 0; ok && c < 4; ++c) {
            const unsigned bgra_channel = c == 0 ? 2 : c == 2 ? 0
                                                              : c;
            uint64_t       total        = 0;

            for (size_t pixel = 0; pixel < size / 4; ++pixel) {
                /*
                 * The expected value: the capture's, or for a channel the format does not carry
                 * its default, 255 for alpha and 0 for the rest.
                 */
                const uint8_t wanted = (format->channel_mask & (1u << c)) != 0 ? expected[pixel * 4 + bgra_channel] : c == 3 ? 255
                                                                                                                             : 0;

                int difference = decoded[pixel * 4 + c] - wanted;

                total += (uint64_t)abs(difference);
            }

            /*
             * The bound a pixel: 0 for an uncompressed format or a channel the format does not
             * carry, otherwise 6 for alpha and 8 for a colour channel.
             */
            const unsigned bound = format->block_bytes == 0 || (format->channel_mask & (1u << c)) == 0 ? 0 : c == 3 ? 6
                                                                                                                    : 8;

            if (total > (uint64_t)bound * (size / 4)) {
                fprintf(stderr, "%s mip %u channel %u absolute error %llu / %zu exceeds %u\n",
                    path, level, c, (unsigned long long)total, size / 4, bound);
                ok = 0;
            }
        }

        free(stored.data);

        /* The next mip's size: halved, but never below 1. */
        width  = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }

    /* The hex holds the chain and nothing more. */
    ok = ok && hex[cursor] == 0;
    free(file.data);

    if (!ok) {
        fprintf(stderr, "Workbench swizzle mismatch: %s (%s)\n", path, format_name);
    }

    return ok;
}

/**
 * Runs the check the arguments name, and exits 0 when it passes, 1 when it fails, and 2 when the
 * arguments fit no form:
 * - `--swizzle FILE WIDTH HEIGHT COUNT HEX FORMAT`: verify_swizzle;
 * - `--golden FILE WIDTH HEIGHT COUNT HEX REMOVED`: verify_golden;
 * - `--gpu SOURCE RESULT FORMAT BOUND`: verify_gpu;
 * - `--mips KAISER PRE POST`: verify_mip_modes;
 * - `COPY LZ4 [PNG TGA JPG TIFF DDS]`: the COPY and LZ4 fixtures, and the five converted sources
 *   when they are given, each against the pixels it must hold.
 */
int main(int argc, char **argv) {
    /* The named forms, each handed to its check. */
    if (argc == 8 && strcmp(argv[1], "--swizzle") == 0) {
        return verify_swizzle(argv[2], (unsigned)strtoul(argv[3], NULL, 10),
                   (unsigned)strtoul(argv[4], NULL, 10), (unsigned)strtoul(argv[5], NULL, 10),
                   argv[6], argv[7])
            ? 0
            : 1;
    }

    if (argc == 8 && strcmp(argv[1], "--golden") == 0) {
        return verify_golden(argv[2], (unsigned)strtoul(argv[3], NULL, 10),
                   (unsigned)strtoul(argv[4], NULL, 10), (unsigned)strtoul(argv[5], NULL, 10),
                   argv[6], (unsigned)strtoul(argv[7], NULL, 10))
            ? 0
            : 1;
    }

    if (argc == 6 && strcmp(argv[1], "--gpu") == 0) {
        return verify_gpu(argv[2], argv[3], argv[4], (unsigned)strtoul(argv[5], NULL, 10)) ? 0 : 1;
    }

    if (argc == 5 && strcmp(argv[1], "--mips") == 0) {
        return verify_mip_modes(argv[2], argv[3], argv[4]) ? 0 : 1;
    }

    /* Otherwise the fixtures: the pixels the COPY and LZ4 ones must hold. */
    static const uint8_t copy_expected[] = { 30, 20, 10, 40 };
    static const uint8_t lz4_expected[]  = { 3, 2, 1, 0, 6, 5, 4, 17 };

    /* The files, one mip read out of them at a time, and the verdict. */
    bytes copy    = { NULL, 0 };
    bytes lz4     = { NULL, 0 };
    bytes decoded = { NULL, 0 };
    bytes png     = { NULL, 0 };
    bytes tga     = { NULL, 0 };
    bytes jpg     = { NULL, 0 };
    bytes tiff    = { NULL, 0 };
    bytes dds     = { NULL, 0 };
    int   ok;

    if (argc != 3 && argc != 8) {
        fputs("usage: edds-reference-reader COPY LZ4 [PNG_RESULT TGA_RESULT JPG_RESULT TIFF_RESULT DDS_RESULT]\n"
              "       edds-reference-reader --mips KAISER PRE_NORMALIZE POST_NORMALIZE\n",
            stderr);
        return 2;
    }

    if (!load(argv[1], &copy) || !load(argv[2], &lz4)) {
        free(copy.data);
        free(lz4.data);
        return 1;
    }

    /* The COPY fixture's 1x1 mip. */
    ok = selected_mip(&copy, 1, 3, 2, 2, "COPY", &decoded) &&
        decoded.size == sizeof copy_expected &&
        memcmp(decoded.data, copy_expected, sizeof copy_expected) == 0;
    free(decoded.data);
    decoded.data = NULL;
    decoded.size = 0;

    /* The LZ4 fixture's one mip. */
    ok = ok &&
        selected_mip(&lz4, 0, 2, 1, 1, "LZ4 ", &decoded) &&
        decoded.size == sizeof lz4_expected &&
        memcmp(decoded.data, lz4_expected, sizeof lz4_expected) == 0;
    free(decoded.data);
    decoded.data = NULL;
    decoded.size = 0;

    /* The converted sources, when their paths are given. */
    if (argc == 8) {
        /*
         * The pixels they must hold: the PNG's two mips, the DDS's two, and the one 3x2 mip the
         * TGA and the TIFF both carry.
         */
        static const uint8_t png_level_zero[] = {
            30, 20, 10, 40, 70, 60, 50, 80, 110, 100, 90, 120,
            130, 120, 110, 140, 170, 160, 150, 180, 210, 200, 190, 220
        };
        static const uint8_t png_level_one[]  = { 120, 110, 100, 130 };
        static const uint8_t dds_level_zero[] = {
            33, 22, 11, 255, 66, 55, 44, 255
        };
        static const uint8_t dds_level_one[]           = { 99, 88, 77, 255 };
        static const uint8_t three_by_two_level_zero[] = {
            30, 20, 10, 255, 60, 50, 40, 255, 90, 80, 70, 255,
            120, 110, 100, 255, 150, 140, 130, 255, 180, 170, 160, 255
        };

        /*
         * The PNG and TGA results, the PNG's with the alpha flag (0x41) and the TGA's without it
         * (0x40), and the PNG's 3x2 mip.
         */
        ok = ok &&
            load(argv[3], &png) &&
            load(argv[4], &tga) &&
            u32le(png.data + 80) == 0x41u &&
            u32le(tga.data + 80) == 0x40u &&
            selected_mip(&png, 0, 3, 2, 2, "COPY", &decoded) &&
            decoded.size == sizeof png_level_zero &&
            memcmp(decoded.data, png_level_zero, sizeof png_level_zero) == 0;
        free(decoded.data);
        decoded.data = NULL;
        decoded.size = 0;

        /* The PNG's 1x1 mip. */
        ok = ok &&
            selected_mip(&png, 1, 3, 2, 2, "COPY", &decoded) &&
            decoded.size == sizeof png_level_one &&
            memcmp(decoded.data, png_level_one, sizeof png_level_one) == 0;
        free(decoded.data);
        decoded.data = NULL;
        decoded.size = 0;

        /* The TGA's one mip. */
        ok = ok &&
            selected_mip(&tga, 0, 3, 2, 1, "COPY", &decoded) &&
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
            /* The full level, filled in below. */
            uint8_t jpeg_level_zero[16 * 8 * 4];

            /* The last mip: the average of the two greys. */
            static const uint8_t jpeg_smallest[] = { 128, 128, 128, 255 };

            /* Grey 78 in the left eight columns, 178 in the right eight, all opaque. */
            for (size_t pixel = 0; pixel < 16u * 8u; ++pixel) {
                const uint8_t grey = pixel % 16u < 8u ? 78u : 178u;

                jpeg_level_zero[pixel * 4u]      = grey;
                jpeg_level_zero[pixel * 4u + 1u] = grey;
                jpeg_level_zero[pixel * 4u + 2u] = grey;
                jpeg_level_zero[pixel * 4u + 3u] = 255u;
            }

            /* The JPEG and TIFF results, both without the alpha flag, and the JPEG's 16x8 mip. */
            ok = ok &&
                load(argv[5], &jpg) &&
                load(argv[6], &tiff) &&
                u32le(jpg.data + 80) == 0x40u &&
                u32le(tiff.data + 80) == 0x40u &&
                selected_mip(&jpg, 0, 16, 8, 5, "COPY", &decoded) &&
                within(&decoded, jpeg_level_zero, sizeof jpeg_level_zero, 2);
            free(decoded.data);
            decoded.data = NULL;
            decoded.size = 0;

            /* The JPEG's last mip, the fifth. */
            ok = ok && selected_mip(&jpg, 4, 16, 8, 5, "COPY", &decoded) && within(&decoded, jpeg_smallest, sizeof jpeg_smallest, 2);
            free(decoded.data);
            decoded.data = NULL;
            decoded.size = 0;
        }

        /* The TIFF's one mip, the same pixels as the TGA's. */
        ok = ok &&
            selected_mip(&tiff, 0, 3, 2, 1, "COPY", &decoded) &&
            decoded.size == sizeof three_by_two_level_zero &&
            memcmp(decoded.data, three_by_two_level_zero, sizeof three_by_two_level_zero) == 0;
        free(decoded.data);
        decoded.data = NULL;
        decoded.size = 0;

        /* The DDS result's two mips, 2x1 and 1x1. */
        ok = ok &&
            load(argv[7], &dds) &&
            selected_mip(&dds, 0, 2, 1, 2, "COPY", &decoded) &&
            decoded.size == sizeof dds_level_zero &&
            memcmp(decoded.data, dds_level_zero, sizeof dds_level_zero) == 0;
        free(decoded.data);
        decoded.data = NULL;
        decoded.size = 0;

        ok = ok &&
            selected_mip(&dds, 1, 2, 1, 2, "COPY", &decoded) &&
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
