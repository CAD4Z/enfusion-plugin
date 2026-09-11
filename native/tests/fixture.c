#include "fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { DDS_HEADER_BYTES = 128 };

static void put_u32(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

static void put_u32be(uint8_t *at, uint32_t value) {
    at[0] = (uint8_t)(value >> 24);
    at[1] = (uint8_t)(value >> 16);
    at[2] = (uint8_t)(value >> 8);
    at[3] = (uint8_t)value;
}

static void put_u16be(uint8_t *at, uint16_t value) {
    at[0] = (uint8_t)(value >> 8);
    at[1] = (uint8_t)value;
}

static uint32_t crc32(const uint8_t *bytes, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t at = 0; at < size; ++at) {
        crc ^= bytes[at];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

static uint32_t adler32(const uint8_t *bytes, size_t size) {
    uint32_t first = 1;
    uint32_t second = 0;
    for (size_t at = 0; at < size; ++at) {
        first = (first + bytes[at]) % 65521u;
        second = (second + first) % 65521u;
    }
    return (second << 16) | first;
}

static size_t png_chunk(uint8_t *output, const char type[4], const uint8_t *data, uint32_t size) {
    put_u32be(output, size);
    memcpy(output + 4, type, 4);
    if (size != 0) {
        memcpy(output + 8, data, size);
    }
    put_u32be(output + 8u + size, crc32(output + 4, 4u + size));
    return 12u + size;
}

static void header(
    uint8_t *bytes,
    uint32_t width,
    uint32_t height,
    uint32_t mips,
    const char four_cc[4],
    uint32_t alpha_mask
) {
    memset(bytes, 0, DDS_HEADER_BYTES);
    memcpy(bytes, "DDS ", 4);
    put_u32(bytes + 4, 124);
    put_u32(bytes + 8, 0x0002100fu);
    put_u32(bytes + 12, height);
    put_u32(bytes + 16, width);
    put_u32(bytes + 20, width * 4u);
    put_u32(bytes + 28, mips);
    memcpy(bytes + 36, "ENF1", 4);
    put_u32(bytes + 76, 32);
    put_u32(bytes + 80, four_cc[0] == '\0' ? (alpha_mask == 0 ? 0x40u : 0x41u) : 0x04u);
    memcpy(bytes + 84, four_cc, 4);
    put_u32(bytes + 88, four_cc[0] == '\0' ? 32u : 0u);
    put_u32(bytes + 92, four_cc[0] == '\0' ? 0x00ff0000u : 0u);
    put_u32(bytes + 96, four_cc[0] == '\0' ? 0x0000ff00u : 0u);
    put_u32(bytes + 100, four_cc[0] == '\0' ? 0x000000ffu : 0u);
    put_u32(bytes + 104, alpha_mask);
    put_u32(bytes + 108, mips > 1u ? 0x00401008u : 0x00001000u);
}

static test_bytes allocated(size_t size) {
    test_bytes fixture = { calloc(1, size), size };
    if (fixture.data == NULL) {
        fixture.size = 0;
    }
    return fixture;
}

test_bytes fixture_copy_bgra(void) {
    static const uint8_t small[] = { 30, 20, 10, 40 };
    static const uint8_t large[] = {
        3, 2, 1, 4, 7, 6, 5, 8, 11, 10, 9, 12,
        15, 14, 13, 16, 19, 18, 17, 20, 23, 22, 21, 24
    };
    test_bytes fixture = allocated(DDS_HEADER_BYTES + 16u + sizeof small + sizeof large);
    if (fixture.data == NULL) {
        return fixture;
    }

    header(fixture.data, 3, 2, 2, "\0\0\0\0", 0xff000000u);
    memcpy(fixture.data + 128, "COPY", 4);
    put_u32(fixture.data + 132, (uint32_t)sizeof small);
    memcpy(fixture.data + 136, "COPY", 4);
    put_u32(fixture.data + 140, (uint32_t)sizeof large);
    memcpy(fixture.data + 144, small, sizeof small);
    memcpy(fixture.data + 148, large, sizeof large);
    return fixture;
}

static size_t literal_block(uint8_t *output, const uint8_t *raw, size_t length) {
    size_t at = 0;
    size_t remaining = length > 15u ? length - 15u : 0u;
    output[at++] = (uint8_t)((length < 15u ? length : 15u) << 4);
    if (length >= 15u) {
        while (remaining >= 255u) {
            output[at++] = 255u;
            remaining -= 255u;
        }
        output[at++] = (uint8_t)remaining;
    }
    memcpy(output + at, raw, length);
    return at + length;
}

test_bytes fixture_lz4_bgrx(void) {
    static const uint8_t raw[] = { 3, 2, 1, 0, 6, 5, 4, 17 };
    uint8_t compressed[32];
    const size_t compressed_size = literal_block(compressed, raw, sizeof raw);
    const uint32_t stored = (uint32_t)(4u + 4u + compressed_size);
    test_bytes fixture = allocated(DDS_HEADER_BYTES + 8u + stored);
    if (fixture.data == NULL) {
        return fixture;
    }

    header(fixture.data, 2, 1, 1, "\0\0\0\0", 0u);
    memcpy(fixture.data + 128, "LZ4 ", 4);
    put_u32(fixture.data + 132, stored);
    put_u32(fixture.data + 136, (uint32_t)sizeof raw);
    put_u32(fixture.data + 140, 0x80000000u | (uint32_t)compressed_size);
    memcpy(fixture.data + 144, compressed, compressed_size);
    return fixture;
}

test_bytes fixture_lz4_streaming_bgrx(void) {
    enum { RAW_BYTES = 256 * 65 * 4, FIRST_BYTES = 65536 };
    uint8_t *raw = malloc(RAW_BYTES);
    uint8_t *first = malloc(FIRST_BYTES + 300u);
    size_t first_size;
    static const uint8_t second[] = { 0x0f, 0x04, 0x00, 0xff, 0xff, 0xff, 0xf0 };
    test_bytes fixture;
    size_t payload;
    size_t at;

    if (raw == NULL || first == NULL) {
        free(raw);
        free(first);
        fixture.data = NULL;
        fixture.size = 0;
        return fixture;
    }
    for (size_t index = 0; index < RAW_BYTES; index += 4u) {
        raw[index] = 3;
        raw[index + 1u] = 2;
        raw[index + 2u] = 1;
        raw[index + 3u] = 99;
    }
    first_size = literal_block(first, raw, FIRST_BYTES);
    payload = 4u + 4u + first_size + 4u + sizeof second;
    fixture = allocated(DDS_HEADER_BYTES + 8u + payload);
    if (fixture.data != NULL) {
        header(fixture.data, 256, 65, 1, "\0\0\0\0", 0u);
        memcpy(fixture.data + 128, "LZ4 ", 4);
        put_u32(fixture.data + 132, (uint32_t)payload);
        at = 136;
        put_u32(fixture.data + at, RAW_BYTES);
        at += 4;
        put_u32(fixture.data + at, (uint32_t)first_size);
        at += 4;
        memcpy(fixture.data + at, first, first_size);
        at += first_size;
        put_u32(fixture.data + at, 0x80000000u | (uint32_t)sizeof second);
        at += 4;
        memcpy(fixture.data + at, second, sizeof second);
    }
    free(raw);
    free(first);
    return fixture;
}

test_bytes fixture_dx10_bgrx(void) {
    static const uint8_t raw[] = { 30, 20, 10, 77, 60, 50, 40, 88 };
    test_bytes fixture = allocated(DDS_HEADER_BYTES + 20u + 8u + sizeof raw);
    if (fixture.data == NULL) {
        return fixture;
    }
    header(fixture.data, 2, 1, 1, "DX10", 0u);
    put_u32(fixture.data + 128, 93u);
    put_u32(fixture.data + 132, 3u);
    put_u32(fixture.data + 136, 0u);
    put_u32(fixture.data + 140, 1u);
    put_u32(fixture.data + 144, 0u);
    memcpy(fixture.data + 148, "COPY", 4);
    put_u32(fixture.data + 152, (uint32_t)sizeof raw);
    memcpy(fixture.data + 156, raw, sizeof raw);
    return fixture;
}

test_bytes fixture_dxt1(void) {
    static const uint8_t block[8] = { 0 };
    test_bytes fixture = allocated(DDS_HEADER_BYTES + 8u + sizeof block);
    if (fixture.data == NULL) {
        return fixture;
    }
    header(fixture.data, 4, 4, 1, "DXT1", 0u);
    memcpy(fixture.data + 128, "COPY", 4);
    put_u32(fixture.data + 132, (uint32_t)sizeof block);
    memcpy(fixture.data + 136, block, sizeof block);
    return fixture;
}

test_bytes fixture_odd_fourcc(void) {
    static const char odd_fourcc[4] = { 'Q', '"', '\\', '\1' };
    static const uint8_t block[8] = { 0 };
    test_bytes fixture = allocated(DDS_HEADER_BYTES + 8u + sizeof block);
    if (fixture.data == NULL) {
        return fixture;
    }
    header(fixture.data, 4, 4, 1, odd_fourcc, 0u);
    memcpy(fixture.data + 128, "COPY", 4);
    put_u32(fixture.data + 132, (uint32_t)sizeof block);
    memcpy(fixture.data + 136, block, sizeof block);
    return fixture;
}

test_bytes fixture_integer_overflow(void) {
    test_bytes fixture = fixture_copy_bgra();
    if (fixture.data != NULL) {
        memset(fixture.data + 12, 0xff, 8);
    }
    return fixture;
}

test_bytes fixture_png_rgba(void) {
    static const uint8_t filtered[] = {
        0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120,
        0, 110, 120, 130, 140, 150, 160, 170, 180, 190, 200, 210, 220
    };
    uint8_t ihdr[13] = { 0 };
    uint8_t idat[2u + 5u + sizeof filtered + 4u];
    test_bytes fixture = allocated(8u + 25u + 12u + sizeof idat + 12u);
    size_t at = 0;
    if (fixture.data == NULL) {
        return fixture;
    }
    memcpy(fixture.data + at, "\x89PNG\r\n\x1a\n", 8);
    at += 8;
    put_u32be(ihdr, 3);
    put_u32be(ihdr + 4, 2);
    ihdr[8] = 8;
    ihdr[9] = 6;
    at += png_chunk(fixture.data + at, "IHDR", ihdr, sizeof ihdr);
    idat[0] = 0x78;
    idat[1] = 0x01;
    idat[2] = 0x01;
    idat[3] = (uint8_t)sizeof filtered;
    idat[4] = 0;
    idat[5] = (uint8_t)(~(uint32_t)sizeof filtered & 0xffu);
    idat[6] = 0xff;
    memcpy(idat + 7, filtered, sizeof filtered);
    put_u32be(idat + 7u + sizeof filtered, adler32(filtered, sizeof filtered));
    at += png_chunk(fixture.data + at, "IDAT", idat, sizeof idat);
    at += png_chunk(fixture.data + at, "IEND", NULL, 0);
    fixture.size = at;
    return fixture;
}

test_bytes fixture_png_rgba_gamma(void) {
    test_bytes base = fixture_png_rgba();
    test_bytes fixture = allocated(base.size + 16u);
    uint8_t gamma[4];
    size_t at = 33u;
    if (base.data == NULL || fixture.data == NULL) {
        fixture_free(base);
        fixture_free(fixture);
        return (test_bytes){ NULL, 0 };
    }
    memcpy(fixture.data, base.data, at);
    put_u32be(gamma, 45455u);
    at += png_chunk(fixture.data + at, "gAMA", gamma, sizeof gamma);
    memcpy(fixture.data + at, base.data + 33u, base.size - 33u);
    fixture.size = base.size + 16u;
    fixture_free(base);
    return fixture;
}

test_bytes fixture_tga_bgrx(void) {
    static const uint8_t source[] = {
        0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        3, 0, 2, 0, 24, 0x20,
        30, 20, 10, 60, 50, 40, 90, 80, 70,
        120, 110, 100, 150, 140, 130, 180, 170, 160
    };
    test_bytes fixture = allocated(sizeof source);
    if (fixture.data != NULL) {
        memcpy(fixture.data, source, sizeof source);
    }
    return fixture;
}

size_t fixture_tiff_ifd_end(size_t tag_count) {
    return 8u + 2u + tag_count * 12u + 4u;
}

size_t fixture_tiff_build(
    uint8_t *output,
    size_t capacity,
    int big_endian,
    const fixture_tiff_tag *tags,
    size_t tag_count,
    const uint8_t *trailing,
    size_t trailing_size
) {
    const size_t ifd_end = fixture_tiff_ifd_end(tag_count);
    const size_t total = ifd_end + trailing_size;
    size_t at;
    if (output == NULL || capacity < total) {
        return 0;
    }
    memset(output, 0, total);
    memcpy(output, big_endian ? "MM\0\x2a" : "II\x2a\0", 4);
    if (big_endian) {
        put_u32be(output + 4, 8);
        output[8] = (uint8_t)(tag_count >> 8);
        output[9] = (uint8_t)tag_count;
    } else {
        put_u32(output + 4, 8);
        output[8] = (uint8_t)tag_count;
        output[9] = (uint8_t)(tag_count >> 8);
    }
    for (at = 0; at < tag_count; ++at) {
        uint8_t *entry = output + 10u + at * 12u;
        const uint32_t element = tags[at].type == 1u || tags[at].type == 2u ? 1u
            : tags[at].type == 3u ? 2u : 4u;
        const int inline_short = tags[at].count == 1u && element == 2u;
        if (big_endian) {
            entry[0] = (uint8_t)(tags[at].tag >> 8);
            entry[1] = (uint8_t)tags[at].tag;
            entry[2] = (uint8_t)(tags[at].type >> 8);
            entry[3] = (uint8_t)tags[at].type;
            put_u32be(entry + 4, tags[at].count);
            if (inline_short) {
                entry[8] = (uint8_t)(tags[at].value >> 8);
                entry[9] = (uint8_t)tags[at].value;
            } else {
                put_u32be(entry + 8, tags[at].value);
            }
        } else {
            entry[0] = (uint8_t)tags[at].tag;
            entry[1] = (uint8_t)(tags[at].tag >> 8);
            entry[2] = (uint8_t)tags[at].type;
            entry[3] = (uint8_t)(tags[at].type >> 8);
            put_u32(entry + 4, tags[at].count);
            put_u32(entry + 8, tags[at].value);
        }
    }
    if (trailing_size != 0) {
        memcpy(output + ifd_end, trailing, trailing_size);
    }
    return total;
}

size_t fixture_jpeg_build(uint8_t *output, size_t capacity, const fixture_jpeg_spec *spec) {
    /*
     * Every quantisation entry is one and both Huffman tables are three symbols wide, so a DC-only
     * block decodes to a flat sample a test can predict exactly: coefficient / 8 + 128.
     */
    static const uint8_t quantisation[65] = { 0 };
    static const uint8_t dc_table[20] = {
        0x00, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x09, 0x0a
    };
    static const uint8_t ac_table[18] = {
        0x10, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00
    };
    const uint8_t components = spec->component_count;
    size_t at = 0;
    if (output == NULL || components == 0u || components > 3u) {
        return 0;
    }
    if (capacity < 256u + spec->exif_size + spec->entropy_size) {
        return 0;
    }
    output[at++] = 0xff;
    output[at++] = 0xd8;
    if (spec->exif_size != 0) {
        output[at++] = 0xff;
        output[at++] = 0xe1;
        put_u16be(output + at, (uint16_t)(spec->exif_size + 2u));
        at += 2;
        memcpy(output + at, spec->exif, spec->exif_size);
        at += spec->exif_size;
    }
    output[at++] = 0xff;
    output[at++] = 0xdb;
    put_u16be(output + at, 67);
    at += 2;
    memcpy(output + at, quantisation, sizeof quantisation);
    for (size_t entry = 1; entry <= 64u; ++entry) {
        output[at + entry] = 1;
    }
    at += sizeof quantisation;
    output[at++] = 0xff;
    output[at++] = spec->frame_marker;
    put_u16be(output + at, (uint16_t)(8u + (size_t)components * 3u));
    at += 2;
    output[at++] = spec->precision;
    put_u16be(output + at, spec->height);
    at += 2;
    put_u16be(output + at, spec->width);
    at += 2;
    output[at++] = components;
    for (uint8_t component = 0; component < components; ++component) {
        output[at++] = (uint8_t)(component + 1u);
        output[at++] = component == 0u ? spec->luma_sampling : 0x11u;
        output[at++] = 0x00;
    }
    output[at++] = 0xff;
    output[at++] = 0xc4;
    put_u16be(output + at, (uint16_t)(sizeof dc_table + 2u));
    at += 2;
    memcpy(output + at, dc_table, sizeof dc_table);
    at += sizeof dc_table;
    output[at++] = 0xff;
    output[at++] = 0xc4;
    put_u16be(output + at, (uint16_t)(sizeof ac_table + 2u));
    at += 2;
    memcpy(output + at, ac_table, sizeof ac_table);
    at += sizeof ac_table;
    if (spec->restart_interval != 0u) {
        output[at++] = 0xff;
        output[at++] = 0xdd;
        put_u16be(output + at, 4);
        at += 2;
        put_u16be(output + at, spec->restart_interval);
        at += 2;
    }
    output[at++] = 0xff;
    output[at++] = 0xda;
    put_u16be(output + at, (uint16_t)(6u + (size_t)components * 2u));
    at += 2;
    output[at++] = components;
    for (uint8_t component = 0; component < components; ++component) {
        output[at++] = (uint8_t)(component + 1u);
        output[at++] = 0x00;
    }
    output[at++] = 0x00;
    output[at++] = 0x3f;
    output[at++] = 0x00;
    memcpy(output + at, spec->entropy, spec->entropy_size);
    at += spec->entropy_size;
    if (!spec->omit_end_of_image) {
        output[at++] = 0xff;
        output[at++] = 0xd9;
    }
    return at;
}

/** 16x8 in two flat DC-only MCUs: grey 78 on the left, grey 178 on the right. */
test_bytes fixture_jpeg_ycbcr(void) {
    static const uint8_t entropy[] = { 0x8d, 0xe0, 0xf2, 0x00, 0x7f };
    fixture_jpeg_spec spec = { .frame_marker = 0xc0, .precision = 8, .width = 16, .height = 8,
          .component_count = 3, .luma_sampling = 0x11,
          .entropy = entropy, .entropy_size = sizeof entropy };
    test_bytes fixture = allocated(512);
    if (fixture.data == NULL) {
        return fixture;
    }
    fixture.size = fixture_jpeg_build(fixture.data, 512, &spec);
    if (fixture.size == 0) {
        fixture_free(fixture);
        return (test_bytes){ NULL, 0 };
    }
    return fixture;
}

/** 3x2 uncompressed RGB, the same pixels the TGA fixture carries, so both agree on the result. */
test_bytes fixture_tiff_rgb(void) {
    static const uint8_t pixels[] = {
        10, 20, 30, 40, 50, 60, 70, 80, 90,
        100, 110, 120, 130, 140, 150, 160, 170, 180
    };
    const uint32_t bits_at = (uint32_t)fixture_tiff_ifd_end(9);
    const uint32_t pixels_at = bits_at + 6u;
    const fixture_tiff_tag tags[9] = {
        { 256, 3, 1, 3 },
        { 257, 3, 1, 2 },
        { 258, 3, 3, bits_at },
        { 259, 3, 1, 1 },
        { 262, 3, 1, 2 },
        { 273, 4, 1, pixels_at },
        { 277, 3, 1, 3 },
        { 278, 3, 1, 2 },
        { 279, 4, 1, (uint32_t)sizeof pixels }
    };
    uint8_t trailing[6 + sizeof pixels];
    test_bytes fixture = allocated(pixels_at + sizeof pixels);
    memset(trailing, 0, sizeof trailing);
    trailing[0] = 8;
    trailing[2] = 8;
    trailing[4] = 8;
    memcpy(trailing + 6, pixels, sizeof pixels);
    if (fixture.data == NULL) {
        return fixture;
    }
    fixture.size = fixture_tiff_build(fixture.data, fixture.size, 0, tags, 9, trailing, sizeof trailing);
    if (fixture.size == 0) {
        fixture_free(fixture);
        return (test_bytes){ NULL, 0 };
    }
    return fixture;
}

void fixture_free(test_bytes fixture) {
    free(fixture.data);
}

int fixture_write(const char *path, test_bytes fixture) {
    FILE *file = fopen(path, "wb");
    size_t written;
    if (file == NULL) {
        return 0;
    }
    written = fwrite(fixture.data, 1, fixture.size, file);
    return fclose(file) == 0 && written == fixture.size;
}
