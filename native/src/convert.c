#include <edds/edds.h>

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

enum {
    DDS_HEADER_BYTES = 128,
    DDSD_CAPS = 0x00000001,
    DDSD_HEIGHT = 0x00000002,
    DDSD_WIDTH = 0x00000004,
    DDSD_PITCH = 0x00000008,
    DDSD_PIXELFORMAT = 0x00001000,
    DDSD_MIPMAPCOUNT = 0x00020000,
    DDPF_ALPHAPIXELS = 0x00000001,
    DDPF_RGB = 0x00000040,
    DDSCAPS_COMPLEX = 0x00000008,
    DDSCAPS_TEXTURE = 0x00001000,
    DDSCAPS_MIPMAP = 0x00400000
};

typedef struct decoded_source {
    uint32_t width;
    uint32_t height;
    int has_alpha;
    uint8_t *rgba;
} decoded_source;

typedef struct generated_mip {
    uint32_t width;
    uint32_t height;
    uint32_t bytes;
    uint8_t *bgra;
    edds_container container;
    uint32_t stored_bytes;
    uint8_t *stored;
} generated_mip;

enum { DEFLATE_MAX_BITS = 15 };

typedef struct bit_reader {
    const uint8_t *bytes;
    size_t size;
    size_t at;
    uint64_t bits;
    unsigned bit_count;
} bit_reader;

typedef struct huffman {
    uint16_t count[DEFLATE_MAX_BITS + 1];
    uint16_t symbol[288];
} huffman;

static void fail(edds_error *error, const char *code, const char *format, ...) {
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

static uint16_t u16le(const uint8_t *at) {
    return (uint16_t)((uint16_t)at[0] | ((uint16_t)at[1] << 8));
}

static uint32_t u32be(const uint8_t *at) {
    return ((uint32_t)at[0] << 24) |
        ((uint32_t)at[1] << 16) |
        ((uint32_t)at[2] << 8) |
        at[3];
}

static void put_u32(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

static int read_all(FILE *input, uint8_t **bytes, size_t *size, edds_error *error) {
    long length;
    uint8_t *allocation;
    *bytes = NULL;
    *size = 0;
    if (fseek(input, 0, SEEK_END) != 0 || (length = ftell(input)) < 0 ||
        fseek(input, 0, SEEK_SET) != 0 || (uint64_t)length > EDDS_MAX_FILE_BYTES) {
        fail(error, "source-size-limit", "The source image could not be measured within the supported limit.");
        return 0;
    }
    allocation = malloc(length == 0 ? 1u : (size_t)length);
    if (allocation == NULL) {
        fail(error, "allocation-failed", "Memory for the source image could not be allocated.");
        return 0;
    }
    if (fread(allocation, 1, (size_t)length, input) != (size_t)length) {
        free(allocation);
        fail(error, "source-read-failed", "The source image could not be read completely.");
        return 0;
    }
    *bytes = allocation;
    *size = (size_t)length;
    return 1;
}

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

static uint32_t png_adler32(const uint8_t *bytes, size_t size) {
    uint32_t first = 1;
    uint32_t second = 0;
    for (size_t at = 0; at < size; ++at) {
        first = (first + bytes[at]) % 65521u;
        second = (second + first) % 65521u;
    }
    return (second << 16) | first;
}

static int take_bits(bit_reader *reader, unsigned count, uint32_t *value) {
    while (reader->bit_count < count) {
        if (reader->at >= reader->size) {
            return 0;
        }
        reader->bits |= (uint64_t)reader->bytes[reader->at++] << reader->bit_count;
        reader->bit_count += 8;
    }
    *value = (uint32_t)(reader->bits & (((uint64_t)1u << count) - 1u));
    reader->bits >>= count;
    reader->bit_count -= count;
    return 1;
}

static void align_bits(bit_reader *reader) {
    const unsigned discard = reader->bit_count & 7u;
    reader->bits >>= discard;
    reader->bit_count -= discard;
}

static int build_huffman(huffman *tree, const uint8_t *lengths, uint32_t symbols) {
    uint16_t offsets[DEFLATE_MAX_BITS + 1];
    int left = 1;
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
    uint32_t code = 0;
    uint32_t first = 0;
    uint32_t index = 0;
    for (unsigned length = 1; length <= DEFLATE_MAX_BITS; ++length) {
        uint32_t bit;
        const uint32_t count = tree->count[length];
        if (!take_bits(reader, 1, &bit)) {
            return 0;
        }
        code |= bit;
        if (code < first + count) {
            *symbol = tree->symbol[index + code - first];
            return 1;
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return 0;
}

static int fixed_trees(huffman *literal, huffman *distance) {
    uint8_t literal_lengths[288];
    uint8_t distance_lengths[32];
    for (uint32_t at = 0; at <= 143; ++at) literal_lengths[at] = 8;
    for (uint32_t at = 144; at <= 255; ++at) literal_lengths[at] = 9;
    for (uint32_t at = 256; at <= 279; ++at) literal_lengths[at] = 7;
    for (uint32_t at = 280; at < 288; ++at) literal_lengths[at] = 8;
    memset(distance_lengths, 5, sizeof distance_lengths);
    return build_huffman(literal, literal_lengths, 288) &&
        build_huffman(distance, distance_lengths, 32);
}

static int dynamic_trees(bit_reader *reader, huffman *literal, huffman *distance) {
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    uint8_t code_lengths[19] = { 0 };
    uint8_t lengths[288 + 32] = { 0 };
    huffman codes;
    uint32_t value;
    uint32_t literal_count;
    uint32_t distance_count;
    uint32_t code_count;
    uint32_t at = 0;
    if (!take_bits(reader, 5, &value)) return 0;
    literal_count = value + 257u;
    if (!take_bits(reader, 5, &value)) return 0;
    distance_count = value + 1u;
    if (!take_bits(reader, 4, &value)) return 0;
    code_count = value + 4u;
    if (literal_count > 286u || distance_count > 32u) {
        return 0;
    }
    for (uint32_t index = 0; index < code_count; ++index) {
        if (!take_bits(reader, 3, &value)) return 0;
        code_lengths[order[index]] = (uint8_t)value;
    }
    if (!build_huffman(&codes, code_lengths, 19)) {
        return 0;
    }
    while (at < literal_count + distance_count) {
        uint32_t symbol;
        uint32_t repeat = 1;
        uint8_t length;
        if (!decode_symbol(reader, &codes, &symbol)) {
            return 0;
        }
        if (symbol <= 15u) {
            length = (uint8_t)symbol;
        } else if (symbol == 16u) {
            if (at == 0 || !take_bits(reader, 2, &value)) return 0;
            repeat = value + 3u;
            length = lengths[at - 1u];
        } else if (symbol == 17u) {
            if (!take_bits(reader, 3, &value)) return 0;
            repeat = value + 3u;
            length = 0;
        } else if (symbol == 18u) {
            if (!take_bits(reader, 7, &value)) return 0;
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
    bit_reader *reader,
    const huffman *literal,
    const huffman *distance,
    uint8_t *output,
    size_t output_size,
    size_t *output_at
) {
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
        size_t length;
        size_t offset;
        if (!decode_symbol(reader, literal, &symbol)) {
            return 0;
        }
        if (symbol < 256u) {
            if (*output_at >= output_size) return 0;
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
        if (!take_bits(reader, length_extra[symbol], &extra)) return 0;
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

static int inflate_zlib(const uint8_t *input, size_t input_size, uint8_t *output, size_t output_size) {
    bit_reader reader;
    size_t output_at = 0;
    int final = 0;
    if (input_size < 6u || (input[0] & 0x0fu) != 8u || (input[0] >> 4) > 7u ||
        (((uint32_t)input[0] << 8) | input[1]) % 31u != 0 || (input[1] & 0x20u) != 0) {
        return 0;
    }
    reader.bytes = input + 2;
    reader.size = input_size - 6u;
    reader.at = 0;
    reader.bits = 0;
    reader.bit_count = 0;
    while (!final) {
        uint32_t value;
        uint32_t type;
        huffman literal;
        huffman distance;
        if (!take_bits(&reader, 1, &value)) return 0;
        final = (int)value;
        if (!take_bits(&reader, 2, &type)) return 0;
        if (type == 0) {
            uint32_t length;
            uint32_t complement;
            align_bits(&reader);
            if (!take_bits(&reader, 16, &length) || !take_bits(&reader, 16, &complement) ||
                (length ^ 0xffffu) != complement || length > output_size - output_at) {
                return 0;
            }
            for (uint32_t at = 0; at < length; ++at) {
                if (!take_bits(&reader, 8, &value)) return 0;
                output[output_at++] = (uint8_t)value;
            }
        } else if (type == 1 || type == 2) {
            const int built = type == 1
                ? fixed_trees(&literal, &distance)
                : dynamic_trees(&reader, &literal, &distance);
            if (!built || !inflate_codes(&reader, &literal, &distance,
                    output, output_size, &output_at)) {
                return 0;
            }
        } else {
            return 0;
        }
    }
    return output_at == output_size && reader.at == reader.size && reader.bit_count < 8u &&
        png_adler32(output, output_size) == u32be(input + input_size - 4u);
}

static uint8_t paeth(uint8_t left, uint8_t above, uint8_t upper_left) {
    const int estimate = (int)left + above - upper_left;
    const int left_distance = abs(estimate - left);
    const int above_distance = abs(estimate - above);
    const int corner_distance = abs(estimate - upper_left);
    if (left_distance <= above_distance && left_distance <= corner_distance) return left;
    return above_distance <= corner_distance ? above : upper_left;
}

static int append_idat(uint8_t **idat, size_t *size, const uint8_t *part, size_t part_size) {
    uint8_t *grown;
    if (part_size == 0) {
        return 1;
    }
    if (part_size > EDDS_MAX_FILE_BYTES - *size) {
        return 0;
    }
    grown = realloc(*idat, *size + part_size);
    if (grown == NULL) {
        return 0;
    }
    memcpy(grown + *size, part, part_size);
    *idat = grown;
    *size += part_size;
    return 1;
}

static edds_status decode_png(FILE *input, decoded_source *image, edds_error *error) {
    static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    uint8_t *file = NULL;
    size_t file_size = 0;
    size_t at = 8;
    uint8_t *idat = NULL;
    size_t idat_size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 0;
    int saw_ihdr = 0;
    int saw_idat = 0;
    int ended_idat = 0;
    int saw_iend = 0;
    uint8_t *filtered = NULL;
    uint8_t *raw = NULL;
    uint8_t *rgba = NULL;
    size_t row_bytes;
    size_t filtered_size;
    edds_status status = EDDS_INVALID_INPUT;

    if (!read_all(input, &file, &file_size, error)) {
        return EDDS_INVALID_INPUT;
    }
    if (file_size < sizeof signature || memcmp(file, signature, sizeof signature) != 0) {
        fail(error, "invalid-png-signature", "The PNG signature is invalid or truncated.");
        goto done;
    }
    while (at < file_size) {
        uint32_t length;
        const uint8_t *type;
        const uint8_t *data;
        if (file_size - at < 12u) {
            fail(error, "truncated-png-chunk", "A PNG chunk header is truncated.");
            goto done;
        }
        length = u32be(file + at);
        type = file + at + 4u;
        data = file + at + 8u;
        if ((size_t)length > file_size - at - 12u) {
            fail(error, "truncated-png-chunk", "A PNG chunk extends beyond the input boundary.");
            goto done;
        }
        if (png_crc32(type, 4u + length) != u32be(data + length)) {
            fail(error, "invalid-png-crc", "A PNG chunk has an invalid CRC.");
            goto done;
        }
        if (memcmp(type, "IHDR", 4) == 0) {
            if (saw_ihdr || at != 8u || length != 13u) {
                fail(error, "invalid-png-ihdr", "PNG must contain one 13-byte IHDR as its first chunk.");
                goto done;
            }
            saw_ihdr = 1;
            width = u32be(data);
            height = u32be(data + 4);
            if (width == 0 || height == 0 || width > EDDS_MAX_DIMENSION ||
                height > EDDS_MAX_DIMENSION) {
                fail(error, "png-dimension-limit", "PNG dimensions must be between 1 and %u.", EDDS_MAX_DIMENSION);
                goto done;
            }
            if (data[8] != 8 || (data[9] != 2 && data[9] != 6) ||
                data[10] != 0 || data[11] != 0 || data[12] != 0) {
                fail(error, "unsupported-png-subtype",
                    "Only non-interlaced 8-bit RGB and RGBA PNG inputs are supported.");
                status = EDDS_UNSUPPORTED_FORMAT;
                goto done;
            }
            channels = data[9] == 6 ? 4u : 3u;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (!saw_ihdr || ended_idat || !append_idat(&idat, &idat_size, data, length)) {
                fail(error, "invalid-png-idat", "PNG IDAT chunks are missing, out of order, or exceed the input limit.");
                goto done;
            }
            saw_idat = 1;
        } else if (memcmp(type, "IEND", 4) == 0) {
            if (!saw_idat || length != 0 || at + 12u != file_size) {
                fail(error, "invalid-png-iend", "PNG must end with one empty IEND chunk.");
                goto done;
            }
            saw_iend = 1;
        } else {
            if (saw_idat) ended_idat = 1;
            if ((type[0] & 0x20u) == 0) {
                fail(error, "unsupported-png-critical-chunk", "The PNG contains an unsupported critical chunk.");
                status = EDDS_UNSUPPORTED_FORMAT;
                goto done;
            }
        }
        at += 12u + length;
    }
    if (!saw_ihdr || !saw_idat || !saw_iend) {
        fail(error, "incomplete-png", "PNG is missing IHDR, IDAT, or IEND.");
        goto done;
    }
    if ((uint64_t)width * channels > SIZE_MAX ||
        (uint64_t)width * channels + 1u > SIZE_MAX / height) {
        fail(error, "png-size-overflow", "The PNG decoded size overflows the supported address space.");
        goto done;
    }
    row_bytes = (size_t)width * channels;
    filtered_size = (row_bytes + 1u) * height;
    if ((uint64_t)width * height * 4u > EDDS_MAX_PREVIEW_BYTES) {
        fail(error, "png-decoded-size-limit", "The PNG exceeds the decoded-image limit.");
        goto done;
    }
    filtered = malloc(filtered_size);
    raw = malloc(row_bytes * height);
    rgba = malloc((size_t)width * height * 4u);
    if (filtered == NULL || raw == NULL || rgba == NULL) {
        fail(error, "allocation-failed", "Memory for the decoded PNG could not be allocated.");
        status = EDDS_INTERNAL_FAILURE;
        goto done;
    }
    if (!inflate_zlib(idat, idat_size, filtered, filtered_size)) {
        fail(error, "invalid-png-deflate", "The PNG IDAT zlib stream is malformed or has the wrong decoded size.");
        goto done;
    }
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t filter = filtered[(row_bytes + 1u) * y];
        const uint8_t *encoded = filtered + (row_bytes + 1u) * y + 1u;
        uint8_t *decoded = raw + row_bytes * y;
        const uint8_t *above = y == 0 ? NULL : decoded - row_bytes;
        if (filter > 4u) {
            fail(error, "unsupported-png-filter", "The PNG scanline uses an unknown filter type.");
            goto done;
        }
        for (size_t x = 0; x < row_bytes; ++x) {
            const uint8_t left = x < channels ? 0 : decoded[x - channels];
            const uint8_t up = above == NULL ? 0 : above[x];
            const uint8_t corner = above == NULL || x < channels ? 0 : above[x - channels];
            uint8_t predictor = 0;
            if (filter == 1) predictor = left;
            else if (filter == 2) predictor = up;
            else if (filter == 3) predictor = (uint8_t)(((uint32_t)left + up) / 2u);
            else if (filter == 4) predictor = paeth(left, up, corner);
            decoded[x] = (uint8_t)(encoded[x] + predictor);
        }
    }
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t source_at = ((size_t)y * width + x) * channels;
            const size_t output_at = ((size_t)y * width + x) * 4u;
            rgba[output_at] = raw[source_at];
            rgba[output_at + 1u] = raw[source_at + 1u];
            rgba[output_at + 2u] = raw[source_at + 2u];
            rgba[output_at + 3u] = channels == 4 ? raw[source_at + 3u] : 255u;
        }
    }
    image->width = width;
    image->height = height;
    image->has_alpha = channels == 4;
    image->rgba = rgba;
    rgba = NULL;
    status = EDDS_OK;

done:
    free(file);
    free(idat);
    free(filtered);
    free(raw);
    free(rgba);
    return status;
}

static edds_status decode_tga(FILE *input, decoded_source *image, edds_error *error) {
    uint8_t *file = NULL;
    size_t size = 0;
    size_t data_at;
    size_t data_bytes;
    uint32_t channels;
    uint32_t attributes;
    uint32_t width;
    uint32_t height;
    uint8_t *rgba;

    if (!read_all(input, &file, &size, error)) {
        return EDDS_INVALID_INPUT;
    }
    if (size < 18u) {
        free(file);
        fail(error, "truncated-tga-header", "The TGA header is truncated.");
        return EDDS_INVALID_INPUT;
    }
    if (file[1] != 0 || file[2] != 2) {
        free(file);
        fail(error, "unsupported-tga-subtype",
            "Only TGA color-map type 0 and uncompressed true-color image type 2 are supported.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    width = u16le(file + 12);
    height = u16le(file + 14);
    if (width == 0 || height == 0 || width > EDDS_MAX_DIMENSION || height > EDDS_MAX_DIMENSION) {
        free(file);
        fail(error, "tga-dimension-limit", "TGA dimensions must be between 1 and %u.", EDDS_MAX_DIMENSION);
        return EDDS_INVALID_INPUT;
    }
    if (file[16] != 24 && file[16] != 32) {
        free(file);
        fail(error, "unsupported-tga-bit-depth", "Only 24-bit and 32-bit true-color TGA inputs are supported.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    attributes = file[17] & 0x0fu;
    if ((file[17] & 0xc0u) != 0 || (file[16] == 24 && attributes != 0) ||
        (file[16] == 32 && attributes != 0 && attributes != 8)) {
        free(file);
        fail(error, "unsupported-tga-descriptor",
            "The TGA descriptor must be non-interleaved with zero or eight alpha bits.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    channels = file[16] / 8u;
    data_at = 18u + file[0];
    data_bytes = (size_t)width * height * channels;
    if (data_at > size || data_bytes > size - data_at ||
        (uint64_t)width * height * 4u > EDDS_MAX_PREVIEW_BYTES) {
        free(file);
        fail(error, "truncated-tga-pixels", "The TGA pixel array is truncated or exceeds the decoded-image limit.");
        return EDDS_INVALID_INPUT;
    }
    rgba = malloc((size_t)width * height * 4u);
    if (rgba == NULL) {
        free(file);
        fail(error, "allocation-failed", "Memory for the decoded TGA could not be allocated.");
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

static uint32_t mip_count(uint32_t width, uint32_t height, int generate) {
    uint32_t count = 1;
    if (!generate) {
        return count;
    }
    while (width > 1 || height > 1) {
        width = width > 1 ? width / 2u : 1u;
        height = height > 1 ? height / 2u : 1u;
        ++count;
    }
    return count;
}

static int mip_bytes(uint32_t width, uint32_t height, uint32_t *bytes) {
    const uint64_t value = (uint64_t)width * height * 4u;
    if (value == 0 || value > EDDS_MAX_PREVIEW_BYTES) {
        return 0;
    }
    *bytes = (uint32_t)value;
    return 1;
}

static void source_mip(const decoded_source *source, generated_mip *mip) {
    for (size_t at = 0; at < mip->bytes; at += 4u) {
        mip->bgra[at] = source->rgba[at + 2u];
        mip->bgra[at + 1u] = source->rgba[at + 1u];
        mip->bgra[at + 2u] = source->rgba[at];
        mip->bgra[at + 3u] = source->has_alpha ? source->rgba[at + 3u] : 255u;
    }
}

static void box_mip(const generated_mip *previous, generated_mip *next) {
    const uint32_t sample_width = previous->width > 1 ? 2u : 1u;
    const uint32_t sample_height = previous->height > 1 ? 2u : 1u;
    const uint32_t divisor = sample_width * sample_height;
    for (uint32_t y = 0; y < next->height; ++y) {
        for (uint32_t x = 0; x < next->width; ++x) {
            const size_t output_at = ((size_t)y * next->width + x) * 4u;
            for (uint32_t channel = 0; channel < 4; ++channel) {
                uint32_t total = 0;
                for (uint32_t dy = 0; dy < sample_height; ++dy) {
                    for (uint32_t dx = 0; dx < sample_width; ++dx) {
                        const size_t source_at =
                            ((size_t)(y * sample_height + dy) * previous->width +
                                x * sample_width + dx) * 4u;
                        total += previous->bgra[source_at + channel];
                    }
                }
                next->bgra[output_at + channel] = (uint8_t)((total + divisor / 2u) / divisor);
            }
        }
    }
}

static uint32_t lz4_hash(const uint8_t *at) {
    const uint32_t value = (uint32_t)at[0] |
        ((uint32_t)at[1] << 8) |
        ((uint32_t)at[2] << 16) |
        ((uint32_t)at[3] << 24);
    return (value * 2654435761u) >> 16;
}

static int write_lz4_length(uint8_t *output, size_t capacity, size_t *at, size_t length) {
    while (length >= 255u) {
        if (*at >= capacity) return 0;
        output[(*at)++] = 255;
        length -= 255u;
    }
    if (*at >= capacity) return 0;
    output[(*at)++] = (uint8_t)length;
    return 1;
}

static int lz4_block(
    const uint8_t *input,
    size_t size,
    unsigned search_depth,
    uint8_t *output,
    size_t capacity,
    size_t *written
) {
    int32_t *head = malloc(65536u * sizeof *head);
    int32_t *previous = malloc((size == 0 ? 1u : size) * sizeof *previous);
    size_t input_at = 0;
    size_t anchor = 0;
    size_t output_at = 0;
    int ok = 0;
    if (head == NULL || previous == NULL) {
        goto done;
    }
    memset(head, 0xff, 65536u * sizeof *head);
    while (input_at + 12u <= size) {
        const uint32_t hash = lz4_hash(input + input_at);
        int32_t candidate = head[hash];
        size_t best_length = 0;
        size_t best_offset = 0;
        unsigned searched = 0;
        previous[input_at] = candidate;
        head[hash] = (int32_t)input_at;
        while (candidate >= 0 && searched++ < search_depth &&
               input_at - (size_t)candidate <= 65535u) {
            size_t length = 0;
            const size_t maximum = size - input_at - 5u;
            while (length < maximum && input[(size_t)candidate + length] == input[input_at + length]) {
                ++length;
            }
            if (length > best_length && length >= 4u) {
                best_length = length;
                best_offset = input_at - (size_t)candidate;
                if (search_depth == 1u || length == maximum) break;
            }
            candidate = previous[candidate];
        }
        if (best_length < 4u) {
            ++input_at;
            continue;
        }
        {
            const size_t literals = input_at - anchor;
            const size_t match_code = best_length - 4u;
            const size_t token_at = output_at++;
            if (token_at >= capacity) goto done;
            output[token_at] = (uint8_t)((literals < 15u ? literals : 15u) << 4);
            if (literals >= 15u &&
                !write_lz4_length(output, capacity, &output_at, literals - 15u)) goto done;
            if (literals > capacity - output_at) goto done;
            memcpy(output + output_at, input + anchor, literals);
            output_at += literals;
            if (capacity - output_at < 2u) goto done;
            output[output_at++] = (uint8_t)best_offset;
            output[output_at++] = (uint8_t)(best_offset >> 8);
            output[token_at] |= (uint8_t)(match_code < 15u ? match_code : 15u);
            if (match_code >= 15u &&
                !write_lz4_length(output, capacity, &output_at, match_code - 15u)) goto done;
        }
        for (size_t index = 1; index < best_length && input_at + index + 4u <= size; ++index) {
            const size_t position = input_at + index;
            const uint32_t inserted_hash = lz4_hash(input + position);
            previous[position] = head[inserted_hash];
            head[inserted_hash] = (int32_t)position;
        }
        input_at += best_length;
        anchor = input_at;
    }
    {
        const size_t literals = size - anchor;
        const size_t token_at = output_at++;
        if (token_at >= capacity) goto done;
        output[token_at] = (uint8_t)((literals < 15u ? literals : 15u) << 4);
        if (literals >= 15u &&
            !write_lz4_length(output, capacity, &output_at, literals - 15u)) goto done;
        if (literals > capacity - output_at) goto done;
        memcpy(output + output_at, input + anchor, literals);
        output_at += literals;
    }
    *written = output_at;
    ok = 1;

done:
    free(head);
    free(previous);
    return ok;
}

static uint8_t *lz4_frame(
    const uint8_t *input,
    uint32_t size,
    edds_format_compress mode,
    uint32_t *stored_bytes
) {
    const uint32_t block_count = (size + 65535u) / 65536u;
    const size_t capacity = 4u + (size_t)block_count * (4u + 16u) + size + size / 255u;
    const unsigned depth = mode == EDDS_COMPRESS_FASTEST ? 1u :
        (mode == EDDS_COMPRESS_MEDIUM ? 16u : 64u);
    uint8_t *frame = malloc(capacity);
    size_t output_at = 4;
    uint32_t input_at = 0;
    if (frame == NULL) {
        return NULL;
    }
    put_u32(frame, size);
    for (uint32_t block = 0; block < block_count; ++block) {
        const uint32_t block_bytes = size - input_at > 65536u ? 65536u : size - input_at;
        const size_t descriptor_at = output_at;
        size_t compressed_size = 0;
        output_at += 4;
        if (!lz4_block(input + input_at, block_bytes, depth, frame + output_at,
                capacity - output_at, &compressed_size) || compressed_size > UINT32_MAX) {
            free(frame);
            return NULL;
        }
        put_u32(frame + descriptor_at, (block + 1u == block_count ? 0x80000000u : 0u) |
            (uint32_t)compressed_size);
        output_at += compressed_size;
        input_at += block_bytes;
    }
    if (output_at > UINT32_MAX) {
        free(frame);
        return NULL;
    }
    *stored_bytes = (uint32_t)output_at;
    return frame;
}

static void report(edds_progress_fn progress, void *context, double value) {
    if (progress != NULL) progress(context, value);
}

static edds_status prepare_storage(
    generated_mip *mips,
    uint32_t count,
    const edds_profile *profile,
    edds_progress_fn progress,
    void *progress_context,
    edds_error *error
) {
    for (uint32_t at = 0; at < count; ++at) {
        generated_mip *mip = &mips[at];
        /* Container compression is the long part of a conversion, so it moves the row per mip. */
        report(progress, progress_context, 0.45 + 0.45 * ((double)at / (double)count));
        mip->container = EDDS_CONTAINER_COPY;
        mip->stored_bytes = mip->bytes;
        mip->stored = mip->bgra;
        if (profile->format_compress != EDDS_COMPRESS_COPY) {
            uint32_t compressed_bytes = 0;
            uint8_t *compressed = lz4_frame(mip->bgra, mip->bytes,
                profile->format_compress, &compressed_bytes);
            if (compressed == NULL) {
                fail(error, "allocation-failed", "Memory for LZ4 container compression could not be allocated.");
                return EDDS_INTERNAL_FAILURE;
            }
            if ((uint64_t)compressed_bytes * 100u <=
                (uint64_t)mip->bytes * profile->compress_threshold) {
                mip->container = EDDS_CONTAINER_LZ4;
                mip->stored_bytes = compressed_bytes;
                mip->stored = compressed;
            } else {
                free(compressed);
            }
        }
    }
    return EDDS_OK;
}

static void free_mips(generated_mip *mips, uint32_t count) {
    for (uint32_t at = 0; at < count; ++at) {
        if (mips[at].stored != mips[at].bgra) {
            free(mips[at].stored);
        }
        free(mips[at].bgra);
        mips[at].bgra = NULL;
        mips[at].stored = NULL;
    }
}

static edds_status generate_mips(
    const decoded_source *source,
    const edds_profile *profile,
    generated_mip *mips,
    uint32_t *count,
    edds_cancelled_fn cancelled,
    void *context,
    edds_error *error
) {
    *count = mip_count(source->width, source->height, profile->generate_mips);
    memset(mips, 0, sizeof(*mips) * *count);
    mips[0].width = source->width;
    mips[0].height = source->height;
    for (uint32_t at = 0; at < *count; ++at) {
        if (cancelled != NULL && cancelled(context)) {
            free_mips(mips, *count);
            fail(error, "cancelled", "The conversion was cancelled.");
            return EDDS_CANCELLED;
        }
        if (at > 0) {
            mips[at].width = mips[at - 1u].width > 1 ? mips[at - 1u].width / 2u : 1u;
            mips[at].height = mips[at - 1u].height > 1 ? mips[at - 1u].height / 2u : 1u;
        }
        if (!mip_bytes(mips[at].width, mips[at].height, &mips[at].bytes)) {
            free_mips(mips, *count);
            fail(error, "mip-size-limit", "A generated mip exceeds the decoded-image limit.");
            return EDDS_INVALID_INPUT;
        }
        mips[at].bgra = malloc(mips[at].bytes);
        if (mips[at].bgra == NULL) {
            free_mips(mips, *count);
            fail(error, "allocation-failed", "Memory for the mip chain could not be allocated.");
            return EDDS_INTERNAL_FAILURE;
        }
        if (at == 0) {
            source_mip(source, &mips[at]);
        } else {
            box_mip(&mips[at - 1u], &mips[at]);
        }
    }
    return EDDS_OK;
}

static void dds_header(uint8_t header[DDS_HEADER_BYTES], const decoded_source *source, uint32_t count) {
    const uint32_t flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH |
        DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT;
    memset(header, 0, DDS_HEADER_BYTES);
    memcpy(header, "DDS ", 4);
    put_u32(header + 4, 124);
    put_u32(header + 8, flags);
    put_u32(header + 12, source->height);
    put_u32(header + 16, source->width);
    put_u32(header + 20, source->width * 4u);
    put_u32(header + 28, count);
    memcpy(header + 36, "ENF1", 4);
    put_u32(header + 76, 32);
    put_u32(header + 80, DDPF_RGB | (source->has_alpha ? DDPF_ALPHAPIXELS : 0u));
    put_u32(header + 88, 32);
    put_u32(header + 92, 0x00ff0000u);
    put_u32(header + 96, 0x0000ff00u);
    put_u32(header + 100, 0x000000ffu);
    put_u32(header + 104, source->has_alpha ? 0xff000000u : 0u);
    put_u32(header + 108, DDSCAPS_TEXTURE |
        (count > 1 ? DDSCAPS_COMPLEX | DDSCAPS_MIPMAP : 0u));
}

static edds_status write_edds(
    FILE *output,
    const decoded_source *source,
    const generated_mip *mips,
    uint32_t count,
    edds_error *error
) {
    uint8_t header[DDS_HEADER_BYTES];
    uint8_t descriptor[8];
    dds_header(header, source, count);
    if (fwrite(header, 1, sizeof header, output) != sizeof header) {
        goto failure;
    }
    for (uint32_t stored = 0; stored < count; ++stored) {
        const generated_mip *mip = &mips[count - stored - 1u];
        memcpy(descriptor, mip->container == EDDS_CONTAINER_COPY ? "COPY" : "LZ4 ", 4);
        put_u32(descriptor + 4, mip->stored_bytes);
        if (fwrite(descriptor, 1, sizeof descriptor, output) != sizeof descriptor) {
            goto failure;
        }
    }
    for (uint32_t stored = 0; stored < count; ++stored) {
        const generated_mip *mip = &mips[count - stored - 1u];
        if (fwrite(mip->stored, 1, mip->stored_bytes, output) != mip->stored_bytes) {
            goto failure;
        }
    }
    if (fflush(output) != 0) {
        goto failure;
    }
    return EDDS_OK;

failure:
    fail(error, "output-write-failed", "The EDDS output could not be written completely.");
    return EDDS_INTERNAL_FAILURE;
}

void edds_default_profile(edds_profile *profile) {
    if (profile != NULL) {
        profile->format_compress = EDDS_COMPRESS_FASTEST;
        profile->compress_threshold = 80;
        profile->generate_mips = 1;
    }
}

edds_status edds_convert(
    FILE *source,
    edds_source_format source_format,
    FILE *output,
    const edds_profile *profile,
    edds_cancelled_fn cancelled,
    void *cancel_context,
    edds_progress_fn progress,
    void *progress_context,
    edds_error *error
) {
    decoded_source image = { 0, 0, 0, NULL };
    generated_mip mips[EDDS_MAX_MIPS];
    uint32_t count = 0;
    edds_status status;
    if (source == NULL || output == NULL || profile == NULL) {
        fail(error, "invalid-api-argument", "The source, output, and profile are required.");
        return EDDS_INTERNAL_FAILURE;
    }
    if (profile->format_compress < EDDS_COMPRESS_COPY ||
        profile->format_compress > EDDS_COMPRESS_BEST || profile->compress_threshold > 100u ||
        (profile->generate_mips != 0 && profile->generate_mips != 1)) {
        fail(error, "unsupported-setting", "The conversion profile is outside the supported Workbench slice.");
        return EDDS_UNSUPPORTED_FORMAT;
    }
    if (source_format == EDDS_SOURCE_TGA) {
        status = decode_tga(source, &image, error);
    } else if (source_format == EDDS_SOURCE_PNG) {
        status = decode_png(source, &image, error);
    } else {
        fail(error, "unsupported-source-format", "Only PNG and TGA source images are supported.");
        status = EDDS_UNSUPPORTED_FORMAT;
    }
    if (status != EDDS_OK) {
        return status;
    }
    report(progress, progress_context, 0.15);
    status = generate_mips(&image, profile, mips, &count, cancelled, cancel_context, error);
    if (status == EDDS_OK) {
        report(progress, progress_context, 0.45);
        status = prepare_storage(mips, count, profile, progress, progress_context, error);
    }
    if (status == EDDS_OK) {
        report(progress, progress_context, 0.90);
        status = write_edds(output, &image, mips, count, error);
    }
    free_mips(mips, count);
    free(image.rgba);
    return status;
}
