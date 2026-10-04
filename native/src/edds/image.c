#include "memory.h"
/*
 * The readers and refusals shared by every source-image codec. Nothing here knows which format it
 * is serving: PNG and TIFF meet in the inflate, JPEG and TIFF in the byte order, all four in the
 * refusal shape and the decoded-size ceiling.
 */
#include "image.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

enum {
    DEFLATE_MAX_BITS = 15
};

typedef struct bit_reader {
    const uint8_t *bytes;
    size_t         size;
    size_t         at;
    uint64_t       bits;
    unsigned       bit_count;
} bit_reader;

typedef struct huffman {
    uint16_t count[DEFLATE_MAX_BITS + 1];
    uint16_t symbol[288];
} huffman;

void edds_fail(edds_error *error, const char *code, const char *format, ...) {
    va_list arguments;
    if (error == NULL) {
        return;
    }
    memset(error, 0, sizeof *error);
    (void)snprintf(error->code, sizeof error->code, "%s", code);
    va_start(arguments, format);
    (void)vsnprintf(error->message, sizeof error->message, format, arguments);
    va_end(arguments);
}

uint16_t edds_u16le(const uint8_t *at) {
    return (uint16_t)((uint16_t)at[0] | ((uint16_t)at[1] << 8));
}

uint16_t edds_u16be(const uint8_t *at) {
    return (uint16_t)(((uint16_t)at[0] << 8) | (uint16_t)at[1]);
}

uint32_t edds_u32le(const uint8_t *at) {
    return (uint32_t)at[0] |
        ((uint32_t)at[1] << 8) |
        ((uint32_t)at[2] << 16) |
        ((uint32_t)at[3] << 24);
}

uint32_t edds_u32be(const uint8_t *at) {
    return ((uint32_t)at[0] << 24) |
        ((uint32_t)at[1] << 16) |
        ((uint32_t)at[2] << 8) |
        at[3];
}

void edds_put_u32le(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

int edds_read_all(FILE *input, uint8_t **bytes, size_t *size, edds_error *error) {
    long     length;
    uint8_t *allocation;
    *bytes = NULL;
    *size  = 0;
    if (fseek(input, 0, SEEK_END) != 0 || (length = ftell(input)) < 0 ||
        fseek(input, 0, SEEK_SET) != 0 || (uint64_t)length > EDDS_MAX_FILE_BYTES) {
        edds_fail(error, "source-size-limit", "The source image could not be measured within the supported limit.");
        return 0;
    }
    allocation = edds_alloc(length == 0 ? 1u : (size_t)length);
    if (allocation == NULL) {
        edds_fail(error, "allocation-failed", "Memory for the source image could not be allocated.");
        return 0;
    }
    if (fread(allocation, 1, (size_t)length, input) != (size_t)length) {
        edds_free(allocation);
        edds_fail(error, "source-read-failed", "The source image could not be read completely.");
        return 0;
    }
    *bytes = allocation;
    *size  = (size_t)length;
    return 1;
}

static uint32_t adler32(const uint8_t *bytes, size_t size) {
    uint32_t first  = 1;
    uint32_t second = 0;
    for (size_t at = 0; at < size; ++at) {
        first  = (first + bytes[at]) % 65521u;
        second = (second + first) % 65521u;
    }
    return (second << 16) | first;
}

static int take_bits(bit_reader *reader, unsigned count, uint32_t *value) {
    while (reader->bit_count < count) {
        if (reader->at >= reader->size) {
            return 0;
        }
        reader->bits      |= (uint64_t)reader->bytes[reader->at++] << reader->bit_count;
        reader->bit_count += 8;
    }
    *value              = (uint32_t)(reader->bits & (((uint64_t)1u << count) - 1u));
    reader->bits      >>= count;
    reader->bit_count  -= count;
    return 1;
}

static void align_bits(bit_reader *reader) {
    const unsigned discard   = reader->bit_count & 7u;
    reader->bits           >>= discard;
    reader->bit_count       -= discard;
}

static int build_huffman(huffman *tree, const uint8_t *lengths, uint32_t symbols) {
    uint16_t offsets[DEFLATE_MAX_BITS + 1];
    int      left = 1;
    memset(tree, 0, sizeof *tree);
    for (uint32_t symbol = 0; symbol < symbols; ++symbol) {
        if (lengths[symbol] > DEFLATE_MAX_BITS) {
            return 0;
        }
        ++tree->count[lengths[symbol]];
    }
    if (tree->count[0] == symbols) {
        return 0;
    }
    for (unsigned bits = 1; bits <= DEFLATE_MAX_BITS; ++bits) {
        left = (left << 1) - tree->count[bits];
        if (left < 0) {
            return 0;
        }
    }
    offsets[1] = 0;
    for (unsigned bits = 1; bits < DEFLATE_MAX_BITS; ++bits) {
        offsets[bits + 1] = (uint16_t)(offsets[bits] + tree->count[bits]);
    }
    for (uint32_t symbol = 0; symbol < symbols; ++symbol) {
        if (lengths[symbol] != 0) {
            tree->symbol[offsets[lengths[symbol]]++] = (uint16_t)symbol;
        }
    }
    return 1;
}

static int decode_symbol(bit_reader *reader, const huffman *tree, uint32_t *symbol) {
    uint32_t code  = 0;
    uint32_t first = 0;
    uint32_t index = 0;
    for (unsigned length = 1; length <= DEFLATE_MAX_BITS; ++length) {
        uint32_t       bit;
        const uint32_t count = tree->count[length];
        if (!take_bits(reader, 1, &bit)) {
            return 0;
        }
        code |= bit;
        if (code < first + count) {
            *symbol = tree->symbol[index + code - first];
            return 1;
        }
        index  += count;
        first   = (first + count) << 1;
        code  <<= 1;
    }
    return 0;
}

static int fixed_trees(huffman *literal, huffman *distance) {
    uint8_t literal_lengths[288];
    uint8_t distance_lengths[32];
    for (uint32_t at = 0; at <= 143; ++at) {
        literal_lengths[at] = 8;
    }
    for (uint32_t at = 144; at <= 255; ++at) {
        literal_lengths[at] = 9;
    }
    for (uint32_t at = 256; at <= 279; ++at) {
        literal_lengths[at] = 7;
    }
    for (uint32_t at = 280; at < 288; ++at) {
        literal_lengths[at] = 8;
    }
    memset(distance_lengths, 5, sizeof distance_lengths);
    return build_huffman(literal, literal_lengths, 288) &&
        build_huffman(distance, distance_lengths, 32);
}

static int dynamic_trees(bit_reader *reader, huffman *literal, huffman *distance) {
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    uint8_t  code_lengths[19]  = { 0 };
    uint8_t  lengths[288 + 32] = { 0 };
    huffman  codes;
    uint32_t value;
    uint32_t literal_count;
    uint32_t distance_count;
    uint32_t code_count;
    uint32_t at = 0;
    if (!take_bits(reader, 5, &value)) {
        return 0;
    }
    literal_count = value + 257u;
    if (!take_bits(reader, 5, &value)) {
        return 0;
    }
    distance_count = value + 1u;
    if (!take_bits(reader, 4, &value)) {
        return 0;
    }
    code_count = value + 4u;
    if (literal_count > 286u || distance_count > 32u) {
        return 0;
    }
    for (uint32_t index = 0; index < code_count; ++index) {
        if (!take_bits(reader, 3, &value)) {
            return 0;
        }
        code_lengths[order[index]] = (uint8_t)value;
    }
    if (!build_huffman(&codes, code_lengths, 19)) {
        return 0;
    }
    while (at < literal_count + distance_count) {
        uint32_t symbol;
        uint32_t repeat = 1;
        uint8_t  length;
        if (!decode_symbol(reader, &codes, &symbol)) {
            return 0;
        }
        if (symbol <= 15u) {
            length = (uint8_t)symbol;
        } else if (symbol == 16u) {
            if (at == 0 || !take_bits(reader, 2, &value)) {
                return 0;
            }
            repeat = value + 3u;
            length = lengths[at - 1u];
        } else if (symbol == 17u) {
            if (!take_bits(reader, 3, &value)) {
                return 0;
            }
            repeat = value + 3u;
            length = 0;
        } else if (symbol == 18u) {
            if (!take_bits(reader, 7, &value)) {
                return 0;
            }
            repeat = value + 11u;
            length = 0;
        } else {
            return 0;
        }
        if (repeat > literal_count + distance_count - at) {
            return 0;
        }
        while (repeat-- != 0) {
            lengths[at++] = length;
        }
    }
    if (lengths[256] == 0 ||
        !build_huffman(literal, lengths, literal_count) ||
        !build_huffman(distance, lengths + literal_count, distance_count)) {
        return 0;
    }
    return 1;
}

static int inflate_codes(
    bit_reader    *reader,
    const huffman *literal,
    const huffman *distance,
    uint8_t       *output,
    size_t         output_size,
    size_t        *output_at) {
    static const uint16_t length_base[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const uint8_t length_extra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const uint16_t distance_base[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
        6145, 8193, 12289, 16385, 24577
    };
    static const uint8_t distance_extra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };
    for (;;) {
        uint32_t symbol;
        uint32_t extra;
        size_t   length;
        size_t   offset;
        if (!decode_symbol(reader, literal, &symbol)) {
            return 0;
        }
        if (symbol < 256u) {
            if (*output_at >= output_size) {
                return 0;
            }
            output[(*output_at)++] = (uint8_t)symbol;
            continue;
        }
        if (symbol == 256u) {
            return 1;
        }
        if (symbol < 257u || symbol > 285u) {
            return 0;
        }
        symbol -= 257u;
        if (!take_bits(reader, length_extra[symbol], &extra)) {
            return 0;
        }
        length = length_base[symbol] + extra;
        if (!decode_symbol(reader, distance, &symbol) || symbol >= 30u ||
            !take_bits(reader, distance_extra[symbol], &extra)) {
            return 0;
        }
        offset = distance_base[symbol] + extra;
        if (offset > *output_at || length > output_size - *output_at) {
            return 0;
        }
        for (size_t copied = 0; copied < length; ++copied) {
            output[*output_at] = output[*output_at - offset];
            ++*output_at;
        }
    }
}

int edds_inflate_zlib(const uint8_t *input, size_t input_size, uint8_t *output, size_t output_size) {
    bit_reader reader;
    size_t     output_at = 0;
    int        final     = 0;
    if (input_size < 6u || (input[0] & 0x0fu) != 8u || (input[0] >> 4) > 7u ||
        (((uint32_t)input[0] << 8) | input[1]) % 31u != 0 || (input[1] & 0x20u) != 0) {
        return 0;
    }
    reader.bytes     = input + 2;
    reader.size      = input_size - 6u;
    reader.at        = 0;
    reader.bits      = 0;
    reader.bit_count = 0;
    while (!final) {
        uint32_t value;
        uint32_t type;
        huffman  literal;
        huffman  distance;
        if (!take_bits(&reader, 1, &value)) {
            return 0;
        }
        final = (int)value;
        if (!take_bits(&reader, 2, &type)) {
            return 0;
        }
        if (type == 0) {
            uint32_t length;
            uint32_t complement;
            align_bits(&reader);
            if (!take_bits(&reader, 16, &length) || !take_bits(&reader, 16, &complement) ||
                (length ^ 0xffffu) != complement || length > output_size - output_at) {
                return 0;
            }
            for (uint32_t at = 0; at < length; ++at) {
                if (!take_bits(&reader, 8, &value)) {
                    return 0;
                }
                output[output_at++] = (uint8_t)value;
            }
        } else if (type == 1 || type == 2) {
            const int built = type == 1
                ? fixed_trees(&literal, &distance)
                : dynamic_trees(&reader, &literal, &distance);
            if (!built || !inflate_codes(&reader, &literal, &distance, output, output_size, &output_at)) {
                return 0;
            }
        } else {
            return 0;
        }
    }
    return output_at == output_size && reader.at == reader.size && reader.bit_count < 8u &&
        adler32(output, output_size) == edds_u32be(input + input_size - 4u);
}

int edds_decoded_size_allowed(uint32_t width, uint32_t height) {
    return width != 0 && height != 0 && width <= EDDS_MAX_DIMENSION &&
        height <= EDDS_MAX_DIMENSION &&
        (uint64_t)width * height * 4u <= EDDS_MAX_PREVIEW_BYTES;
}
