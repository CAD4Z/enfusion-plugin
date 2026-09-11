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

int main(int argc, char **argv) {
    static const uint8_t copy_expected[] = { 30, 20, 10, 40 };
    static const uint8_t lz4_expected[] = { 3, 2, 1, 0, 6, 5, 4, 17 };
    bytes copy = { NULL, 0 };
    bytes lz4 = { NULL, 0 };
    bytes decoded = { NULL, 0 };
    bytes png = { NULL, 0 };
    bytes tga = { NULL, 0 };
    bytes jpg = { NULL, 0 };
    bytes tiff = { NULL, 0 };
    int ok;
    if (argc != 3 && argc != 7) {
        fputs("usage: edds-reference-reader COPY LZ4 [PNG_RESULT TGA_RESULT JPG_RESULT TIFF_RESULT]\n", stderr);
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
    if (argc == 7) {
        static const uint8_t png_level_zero[] = {
            30, 20, 10, 40, 70, 60, 50, 80, 110, 100, 90, 120,
            130, 120, 110, 140, 170, 160, 150, 180, 210, 200, 190, 220
        };
        static const uint8_t png_level_one[] = { 100, 90, 80, 110 };
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
    }
    free(copy.data);
    free(lz4.data);
    free(png.data);
    free(tga.data);
    free(jpg.data);
    free(tiff.data);
    if (!ok) {
        fputs("independent EDDS reference reader disagreed with the fixture\n", stderr);
    }
    return ok ? 0 : 1;
}
