#include <edds/edds.h>
#include <edds/batch.h>
#include <edds/pool.h>

#include "fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
        return 0; \
    } \
} while (0)

static FILE *temporary(void) {
#ifdef _WIN32
    FILE *file = NULL;
    return tmpfile_s(&file) == 0 ? file : NULL;
#else
    return tmpfile();
#endif
}

static FILE *stream_of(const uint8_t *bytes, size_t size) {
    FILE *file = temporary();
    if (file == NULL) {
        return NULL;
    }
    if (fwrite(bytes, 1, size, file) != size || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    return file;
}

static int never_cancelled(void *context) {
    (void)context;
    return 0;
}

static int always_cancelled(void *context) {
    (void)context;
    return 1;
}

static int copy_inspection_and_preview(void) {
    test_bytes fixture = fixture_copy_bgra();
    FILE *file = stream_of(fixture.data, fixture.size);
    edds_info info;
    edds_error error;
    uint8_t *rgba = NULL;
    size_t size = 0;
    static const uint8_t expected_first_two[] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    CHECK(fixture.data != NULL && file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.width == 3 && info.height == 2 && info.mip_count == 2);
    CHECK(info.pixel_format == EDDS_PIXEL_BGRA8 && info.header_bytes == 128);
    CHECK(info.mips[0].level == 0 && info.mips[0].width == 3 && info.mips[0].height == 2);
    CHECK(info.mips[0].container == EDDS_CONTAINER_COPY);
    CHECK(info.mips[0].stored_bytes == 24 && info.mips[0].data_offset == 148);
    CHECK(info.mips[1].level == 1 && info.mips[1].data_offset == 144);
    CHECK(edds_preview(file, &info, 0, never_cancelled, NULL, &rgba, &size, &error) == EDDS_OK);
    CHECK(size == 24 && memcmp(rgba, expected_first_two, sizeof expected_first_two) == 0);

    edds_free(rgba);
    fclose(file);
    fixture_free(fixture);
    return 1;
}

static int lz4_preview_and_bgrx_alpha(void) {
    test_bytes fixture = fixture_lz4_bgrx();
    FILE *file = stream_of(fixture.data, fixture.size);
    edds_info info;
    edds_error error;
    uint8_t *rgba = NULL;
    size_t size = 0;
    static const uint8_t expected[] = { 1, 2, 3, 255, 4, 5, 6, 255 };

    CHECK(fixture.data != NULL && file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.pixel_format == EDDS_PIXEL_BGRX8);
    CHECK(info.mips[0].container == EDDS_CONTAINER_LZ4 && info.mips[0].block_count == 1);
    CHECK(edds_preview(file, &info, 0, never_cancelled, NULL, &rgba, &size, &error) == EDDS_OK);
    CHECK(size == sizeof expected && memcmp(rgba, expected, sizeof expected) == 0);

    edds_free(rgba);
    fclose(file);
    fixture_free(fixture);
    return 1;
}

static int lz4_stream_keeps_the_previous_dictionary(void) {
    test_bytes fixture = fixture_lz4_streaming_bgrx();
    FILE *file = stream_of(fixture.data, fixture.size);
    edds_info info;
    edds_error error;
    uint8_t *rgba = NULL;
    size_t size = 0;

    CHECK(fixture.data != NULL && file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.mips[0].block_count == 2 && info.mips[0].decoded_bytes == 256u * 65u * 4u);
    CHECK(edds_preview(file, &info, 0, never_cancelled, NULL, &rgba, &size, &error) == EDDS_OK);
    CHECK(size == 256u * 65u * 4u);
    CHECK(rgba[0] == 1 && rgba[1] == 2 && rgba[2] == 3 && rgba[3] == 255);
    CHECK(rgba[size - 4u] == 1 && rgba[size - 1u] == 255);

    edds_free(rgba);
    fclose(file);
    fixture_free(fixture);
    return 1;
}

static int dx10_bgrx_is_a_single_surface_preview(void) {
    test_bytes fixture = fixture_dx10_bgrx();
    FILE *file = stream_of(fixture.data, fixture.size);
    edds_info info;
    edds_error error;
    uint8_t *rgba = NULL;
    size_t size = 0;
    static const uint8_t expected[] = { 10, 20, 30, 255, 40, 50, 60, 255 };

    CHECK(fixture.data != NULL && file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.header_bytes == 148 && info.dxgi_format == 93);
    CHECK(info.resource_dimension == 3 && info.array_size == 1);
    CHECK(info.pixel_format == EDDS_PIXEL_BGRX8 && info.preview_supported);
    CHECK(edds_preview(file, &info, 0, never_cancelled, NULL, &rgba, &size, &error) == EDDS_OK);
    CHECK(size == sizeof expected && memcmp(rgba, expected, sizeof expected) == 0);

    edds_free(rgba);
    fclose(file);
    fixture_free(fixture);
    return 1;
}

static int unsupported_pixels_keep_the_inspection(void) {
    test_bytes fixture = fixture_dxt1();
    FILE *file = stream_of(fixture.data, fixture.size);
    edds_info info;
    edds_error error;
    uint8_t *rgba = NULL;
    size_t size = 0;

    CHECK(fixture.data != NULL && file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.pixel_format == EDDS_PIXEL_DXT1 && info.mips[0].stored_bytes == 8);
    CHECK(edds_preview(file, &info, 0, never_cancelled, NULL, &rgba, &size, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(rgba == NULL && strstr(error.code, "unsupported") != NULL);

    fclose(file);
    fixture_free(fixture);
    return 1;
}

static int contradictory_legacy_flags_never_produce_pixels(void) {
    test_bytes bgra = fixture_copy_bgra();
    test_bytes bgrx = fixture_lz4_bgrx();
    edds_info info;
    edds_error error;
    FILE *file;

    CHECK(bgra.data != NULL && bgrx.data != NULL);
    bgra.data[80] = 0x40;
    file = stream_of(bgra.data, bgra.size);
    CHECK(file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.pixel_format == EDDS_PIXEL_UNKNOWN && !info.preview_supported);
    fclose(file);

    bgrx.data[80] = 0x41;
    file = stream_of(bgrx.data, bgrx.size);
    CHECK(file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.pixel_format == EDDS_PIXEL_UNKNOWN && !info.preview_supported);
    fclose(file);

    fixture_free(bgra);
    fixture_free(bgrx);
    return 1;
}

static int truncation_and_limits_fail_without_output(void) {
    test_bytes fixture = fixture_copy_bgra();
    edds_info info;
    edds_error error;

    CHECK(fixture.data != NULL);
    for (size_t length = 0; length < fixture.size; ++length) {
        FILE *file = stream_of(fixture.data, length);
        CHECK(file != NULL);
        CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_INVALID_INPUT);
        fclose(file);
    }

    fixture.data[16] = 1;
    fixture.data[17] = 0;
    fixture.data[18] = 1;
    fixture.data[19] = 0;
    {
        FILE *file = stream_of(fixture.data, fixture.size);
        CHECK(file != NULL);
        CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_INVALID_INPUT);
        fclose(file);
    }

    fixture_free(fixture);
    return 1;
}

static int malformed_lz4_and_cancellation_are_bounded(void) {
    test_bytes fixture = fixture_lz4_bgrx();
    edds_info info;
    edds_error error;
    FILE *file;

    CHECK(fixture.data != NULL);
    file = stream_of(fixture.data, fixture.size);
    CHECK(file != NULL);
    CHECK(edds_inspect(file, &info, always_cancelled, NULL, &error) == EDDS_CANCELLED);
    fclose(file);

    fixture.data[143] = 0;
    file = stream_of(fixture.data, fixture.size);
    CHECK(file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_INVALID_INPUT);
    fclose(file);

    fixture_free(fixture);
    return 1;
}

static int malformed_sizes_and_oversized_files_are_refused(void) {
    test_bytes copy = fixture_copy_bgra();
    test_bytes lz4 = fixture_lz4_bgrx();
    edds_info info;
    edds_error error;
    FILE *file;

    CHECK(copy.data != NULL && lz4.data != NULL);
    memset(copy.data + 132, 0xff, 4);
    file = stream_of(copy.data, copy.size);
    CHECK(file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_INVALID_INPUT);
    fclose(file);

    memset(lz4.data + 136, 0xff, 4);
    file = stream_of(lz4.data, lz4.size);
    CHECK(file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_INVALID_INPUT);
    fclose(file);

    file = temporary();
    CHECK(file != NULL);
    CHECK(fseek(file, (long)(EDDS_MAX_FILE_BYTES + 1u), SEEK_SET) == 0);
    CHECK(fputc(0, file) != EOF);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_INVALID_INPUT);
    fclose(file);

    fixture_free(copy);
    fixture_free(lz4);
    return 1;
}

static int integer_boundaries_are_refused_before_arithmetic(void) {
    test_bytes dimensions = fixture_integer_overflow();
    test_bytes mips = fixture_copy_bgra();
    edds_info info;
    edds_error error;
    FILE *file;

    CHECK(dimensions.data != NULL && mips.data != NULL);
    file = stream_of(dimensions.data, dimensions.size);
    CHECK(file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_INVALID_INPUT);
    CHECK(strcmp(error.code, "dimension-limit") == 0);
    fclose(file);

    memset(mips.data + 28, 0xff, 4);
    file = stream_of(mips.data, mips.size);
    CHECK(file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_INVALID_INPUT);
    CHECK(strcmp(error.code, "mip-count-limit") == 0);
    fclose(file);

    fixture_free(dimensions);
    fixture_free(mips);
    return 1;
}

static int uncompressed_tga_converts_through_the_public_edds_seam(void) {
    static const uint8_t tga[] = {
        0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        3, 0, 2, 0, 24, 0x20,
        30, 20, 10, 60, 50, 40, 90, 80, 70,
        120, 110, 100, 150, 140, 130, 180, 170, 160
    };
    static const uint8_t expected_mip_zero[] = {
        10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255,
        100, 110, 120, 255, 130, 140, 150, 255, 160, 170, 180, 255
    };
    static const uint8_t expected_mip_one[] = { 70, 80, 90, 255 };
    FILE *source = stream_of(tga, sizeof tga);
    FILE *output = temporary();
    edds_profile profile;
    edds_info info;
    edds_error error;
    uint8_t *rgba = NULL;
    size_t rgba_size = 0;

    CHECK(source != NULL && output != NULL);
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    CHECK(edds_convert(source, EDDS_SOURCE_TGA, output, &profile,
        never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.width == 3 && info.height == 2 && info.mip_count == 2);
    CHECK(info.pixel_format == EDDS_PIXEL_BGRX8);
    CHECK(info.mips[0].container == EDDS_CONTAINER_COPY);
    CHECK(info.mips[1].width == 1 && info.mips[1].height == 1);
    CHECK(edds_preview(output, &info, 0, never_cancelled, NULL,
        &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == sizeof expected_mip_zero &&
        memcmp(rgba, expected_mip_zero, sizeof expected_mip_zero) == 0);
    edds_free(rgba);
    rgba = NULL;
    CHECK(edds_preview(output, &info, 1, never_cancelled, NULL,
        &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == sizeof expected_mip_one &&
        memcmp(rgba, expected_mip_one, sizeof expected_mip_one) == 0);

    edds_free(rgba);
    fclose(source);
    fclose(output);
    return 1;
}

static int supported_workbench_defaults_are_explicit(void) {
    edds_profile profile;
    edds_default_profile(&profile);
    CHECK(profile.format_compress == EDDS_COMPRESS_FASTEST);
    CHECK(profile.compress_threshold == 80u);
    CHECK(profile.generate_mips == 1);
    return 1;
}

static int rgba_png_converts_with_declared_alpha(void) {
    static const uint8_t expected_mip_zero[] = {
        10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120,
        110, 120, 130, 140, 150, 160, 170, 180, 190, 200, 210, 220
    };
    static const uint8_t expected_mip_one[] = { 80, 90, 100, 110 };
    test_bytes png = fixture_png_rgba();
    FILE *source = stream_of(png.data, png.size);
    FILE *output = temporary();
    edds_profile profile;
    edds_info info;
    edds_error error;
    uint8_t *rgba = NULL;
    size_t rgba_size = 0;

    CHECK(png.data != NULL && source != NULL && output != NULL);
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    CHECK(edds_convert(source, EDDS_SOURCE_PNG, output, &profile,
        never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.pixel_format == EDDS_PIXEL_BGRA8 && info.mip_count == 2);
    CHECK(edds_preview(output, &info, 0, never_cancelled, NULL,
        &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == sizeof expected_mip_zero &&
        memcmp(rgba, expected_mip_zero, sizeof expected_mip_zero) == 0);
    edds_free(rgba);
    rgba = NULL;
    CHECK(edds_preview(output, &info, 1, never_cancelled, NULL,
        &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == sizeof expected_mip_one &&
        memcmp(rgba, expected_mip_one, sizeof expected_mip_one) == 0);

    edds_free(rgba);
    fclose(source);
    fclose(output);
    fixture_free(png);
    return 1;
}

static int png_gamma_is_metadata_not_a_sample_transform(void) {
    static const uint8_t expected_first_pixel[] = { 10, 20, 30, 40 };
    test_bytes png = fixture_png_rgba_gamma();
    FILE *source = stream_of(png.data, png.size);
    FILE *output = temporary();
    edds_profile profile;
    edds_info info;
    edds_error error;
    uint8_t *rgba = NULL;
    size_t rgba_size = 0;
    CHECK(png.data != NULL && source != NULL && output != NULL);
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    profile.generate_mips = 0;
    CHECK(edds_convert(source, EDDS_SOURCE_PNG, output, &profile,
        never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(edds_preview(output, &info, 0, never_cancelled, NULL,
        &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == 24u && memcmp(rgba, expected_first_pixel, sizeof expected_first_pixel) == 0);
    edds_free(rgba);
    fclose(source);
    fclose(output);
    fixture_free(png);
    return 1;
}

static int tga_origin_channels_and_declared_alpha_are_normalized(void) {
    static const uint8_t tga[] = {
        0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        2, 0, 2, 0, 32, 0x18,
        12, 11, 10, 13, 9, 8, 7, 10,
        6, 5, 4, 7, 3, 2, 1, 4
    };
    static const uint8_t expected[] = {
        1, 2, 3, 4, 4, 5, 6, 7,
        7, 8, 9, 10, 10, 11, 12, 13
    };
    FILE *source = stream_of(tga, sizeof tga);
    FILE *output = temporary();
    edds_profile profile;
    edds_info info;
    edds_error error;
    uint8_t *rgba = NULL;
    size_t rgba_size = 0;
    CHECK(source != NULL && output != NULL);
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    profile.generate_mips = 0;
    CHECK(edds_convert(source, EDDS_SOURCE_TGA, output, &profile,
        never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.pixel_format == EDDS_PIXEL_BGRA8);
    CHECK(edds_preview(output, &info, 0, never_cancelled, NULL,
        &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == sizeof expected && memcmp(rgba, expected, sizeof expected) == 0);
    edds_free(rgba);
    fclose(source);
    fclose(output);
    return 1;
}

static int format_compress_selects_lossless_lz4_only_at_the_threshold(void) {
    enum { WIDTH = 25, TGA_BYTES = 18 + WIDTH * 3 };
    uint8_t tga[TGA_BYTES] = { 0 };
    edds_profile profile;
    uint32_t fastest_stored = 0;

    tga[2] = 2;
    tga[12] = WIDTH;
    tga[14] = 1;
    tga[16] = 24;
    tga[17] = 0x20;
    for (size_t at = 18; at < sizeof tga; at += 3u) {
        tga[at] = 30;
        tga[at + 1u] = 20;
        tga[at + 2u] = 10;
    }

    edds_default_profile(&profile);
    profile.generate_mips = 0;
    for (edds_format_compress mode = EDDS_COMPRESS_FASTEST;
         mode <= EDDS_COMPRESS_BEST;
         mode = (edds_format_compress)(mode + 1)) {
        FILE *source = stream_of(tga, sizeof tga);
        FILE *output = temporary();
        edds_info info;
        edds_error error;
        uint8_t *rgba = NULL;
        size_t rgba_size = 0;
        CHECK(source != NULL && output != NULL);
        profile.format_compress = mode;
        profile.compress_threshold = 100;
        CHECK(edds_convert(source, EDDS_SOURCE_TGA, output, &profile,
            never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);
        CHECK(fseek(output, 0, SEEK_SET) == 0);
        CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
        CHECK(info.mips[0].container == EDDS_CONTAINER_LZ4);
        if (mode == EDDS_COMPRESS_FASTEST) fastest_stored = info.mips[0].stored_bytes;
        CHECK(edds_preview(output, &info, 0, never_cancelled, NULL,
            &rgba, &rgba_size, &error) == EDDS_OK);
        CHECK(rgba_size == WIDTH * 4u && rgba[0] == 10 && rgba[1] == 20 &&
            rgba[2] == 30 && rgba[3] == 255 &&
            memcmp(rgba, rgba + 4, rgba_size - 4u) == 0);
        edds_free(rgba);
        fclose(source);
        fclose(output);
    }
    CHECK(fastest_stored > 0 && (fastest_stored * 100u) % (WIDTH * 4u) == 0);
    {
        const uint32_t equality = fastest_stored * 100u / (WIDTH * 4u);
        for (unsigned below = 0; below < 2u; ++below) {
            FILE *source = stream_of(tga, sizeof tga);
            FILE *output = temporary();
            edds_info info;
            edds_error error;
            CHECK(source != NULL && output != NULL && equality > 0u);
            profile.format_compress = EDDS_COMPRESS_FASTEST;
            profile.compress_threshold = equality - below;
            CHECK(edds_convert(source, EDDS_SOURCE_TGA, output, &profile,
                never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);
            CHECK(fseek(output, 0, SEEK_SET) == 0);
            CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
            CHECK(info.mips[0].container ==
                (below == 0u ? EDDS_CONTAINER_LZ4 : EDDS_CONTAINER_COPY));
            fclose(source);
            fclose(output);
        }
    }
    {
        FILE *source = stream_of(tga, sizeof tga);
        FILE *output = temporary();
        edds_info info;
        edds_error error;
        CHECK(source != NULL && output != NULL);
        profile.format_compress = EDDS_COMPRESS_FASTEST;
        profile.compress_threshold = 0;
        CHECK(edds_convert(source, EDDS_SOURCE_TGA, output, &profile,
            never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);
        CHECK(fseek(output, 0, SEEK_SET) == 0);
        CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
        CHECK(info.mips[0].container == EDDS_CONTAINER_COPY);
        fclose(source);
        fclose(output);
    }
    return 1;
}

static int compression_efforts_are_observably_distinct_and_lossless(void) {
    enum { PATTERN = 128, SHORT = 8, MEDIUM = 52, PIXEL_BYTES = 468 };
    uint8_t tga[18 + PIXEL_BYTES];
    uint8_t pattern[PATTERN];
    uint32_t stored[3] = { 0, 0, 0 };
    size_t at = 18;
    memset(tga, 0, sizeof tga);
    tga[2] = 2;
    tga[12] = (uint8_t)(PIXEL_BYTES / 4);
    tga[14] = 1;
    tga[16] = 32;
    tga[17] = 0x28;
    for (size_t index = 0; index < PATTERN; ++index) {
        pattern[index] = (uint8_t)((index * 37u + index * index * 13u) % 251u);
    }
    memcpy(tga + at, pattern, PATTERN);
    at += PATTERN;
    for (unsigned decoy = 0; decoy < 12u; ++decoy) {
        memcpy(tga + at, pattern, 4);
        memset(tga + at + 4u, (int)(decoy + 1u), SHORT - 4u);
        at += SHORT;
    }
    memcpy(tga + at, pattern, MEDIUM - 4u);
    memset(tga + at + MEDIUM - 4u, 0xfe, 4u);
    at += MEDIUM;
    for (unsigned decoy = 0; decoy < 8u; ++decoy) {
        memcpy(tga + at, pattern, 4);
        memset(tga + at + 4u, (int)(decoy + 101u), SHORT - 4u);
        at += SHORT;
    }
    memcpy(tga + at, pattern, PATTERN);
    at += PATTERN;
    CHECK(at == sizeof tga);

    for (edds_format_compress mode = EDDS_COMPRESS_FASTEST;
         mode <= EDDS_COMPRESS_BEST;
         mode = (edds_format_compress)(mode + 1)) {
        FILE *source = stream_of(tga, sizeof tga);
        FILE *output = temporary();
        edds_profile profile;
        edds_info info;
        edds_error error;
        uint8_t *rgba = NULL;
        size_t rgba_size = 0;
        CHECK(source != NULL && output != NULL);
        edds_default_profile(&profile);
        profile.generate_mips = 0;
        profile.format_compress = mode;
        profile.compress_threshold = 100;
        CHECK(edds_convert(source, EDDS_SOURCE_TGA, output, &profile,
            never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);
        CHECK(fseek(output, 0, SEEK_SET) == 0);
        CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
        CHECK(info.mips[0].container == EDDS_CONTAINER_LZ4);
        stored[mode - EDDS_COMPRESS_FASTEST] = info.mips[0].stored_bytes;
        CHECK(edds_preview(output, &info, 0, never_cancelled, NULL,
            &rgba, &rgba_size, &error) == EDDS_OK);
        CHECK(rgba_size == PIXEL_BYTES);
        edds_free(rgba);
        fclose(source);
        fclose(output);
    }
    CHECK(stored[0] > stored[1] && stored[1] > stored[2]);
    return 1;
}

static int metadata_round_trip_is_canonical_and_preserves_identity(void) {
    static const char source[] =
        "MetaFileClass {\n"
        " // Layout and unknown fields are intentionally not durable.\n"
        " Name \"{a1B2c3D4e5F60718}Probe/Images/pixel.edds\"\n"
        " Author \"someone\"\n"
        " UnknownTop 42\n"
        " Configurations {\n"
        "  PNGResourceClass PC {\n"
        "   SourceFile \"pixel.png\"\n"
        "   TargetFormat EnfusionDDS\n"
        "   FormatCompress Best\n"
        "   CompressTreshold 73\n"
        "   Conversion None\n"
        "   ConversionQuality 1\n"
        "   Swizzling None\n"
        "   GenerateMips 0\n"
        "   MipMapFunction Filter\n"
        "   MipMapFilter Box\n"
        "   TiledTexture 1\n"
        "   UnknownRecipe \"discard me\"\n"
        "  }\n"
        "  PNGResourceClass XBOX_ONE : PC { Override 1 }\n"
        " }\n"
        "}\n";
    static const char expected[] =
        "MetaFileClass {\n"
        " Name \"{a1B2c3D4e5F60718}Probe/Images/pixel.edds\"\n"
        " Configurations {\n"
        "  PNGResourceClass PC {\n"
        "   SourceFile \"pixel.png\"\n"
        "   TargetFormat EnfusionDDS\n"
        "   FormatCompress Best\n"
        "   CompressTreshold 73\n"
        "   Conversion None\n"
        "   ConversionQuality 1\n"
        "   Swizzling None\n"
        "   GenerateMips 0\n"
        "   MipMapFunction Filter\n"
        "   MipMapFilter Box\n"
        "   TiledTexture 1\n"
        "  }\n"
        "  PNGResourceClass XBOX_ONE : PC {\n"
        "  }\n"
        "  PNGResourceClass PS4 : PC {\n"
        "  }\n"
        "  PNGResourceClass LINUX : PC {\n"
        "  }\n"
        " }\n"
        "}\n";
    FILE *input = stream_of((const uint8_t *)source, strlen(source));
    FILE *output = temporary();
    edds_metadata metadata;
    edds_metadata reparsed;
    edds_error error;
    char actual[sizeof expected];

    CHECK(input != NULL && output != NULL);
    CHECK(edds_metadata_parse(input, &metadata, &error) == EDDS_OK);
    CHECK(strcmp(metadata.guid, "a1B2c3D4e5F60718") == 0);
    CHECK(strcmp(metadata.name, "Probe/Images/pixel.edds") == 0);
    CHECK(strcmp(metadata.source_file, "pixel.png") == 0);
    CHECK(metadata.source_format == EDDS_SOURCE_PNG);
    CHECK(metadata.profile.format_compress == EDDS_COMPRESS_BEST);
    CHECK(metadata.profile.compress_threshold == 73 && !metadata.profile.generate_mips);
    CHECK(edds_metadata_write(output, &metadata, &error) == EDDS_OK);
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(fread(actual, 1, sizeof expected - 1u, output) == sizeof expected - 1u);
    actual[sizeof expected - 1u] = '\0';
    CHECK(strcmp(actual, expected) == 0);
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_metadata_parse(output, &reparsed, &error) == EDDS_OK);
    CHECK(memcmp(&metadata, &reparsed, sizeof metadata) == 0);

    fclose(input);
    fclose(output);
    return 1;
}

static int known_but_unsupported_metadata_is_never_defaulted(void) {
    static const char source[] =
        "MetaFileClass { Name \"{0123456789ABCDEF}a.edds\" Configurations { "
        "TGAResourceClass PC { SourceFile \"a.tga\" Conversion DXTCompression } } }";
    FILE *input = stream_of((const uint8_t *)source, strlen(source));
    edds_metadata metadata;
    edds_error error;
    CHECK(input != NULL);
    CHECK(edds_metadata_parse(input, &metadata, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-setting") == 0);
    fclose(input);
    return 1;
}

/** Every registered resource class parses back to its own format, and only to its own. */
static int every_resource_class_round_trips_through_metadata(void) {
    size_t count = 0;
    const edds_source_capability *capabilities = edds_source_capabilities(&count);
    CHECK(count == 4u);
    for (size_t at = 0; at < count; ++at) {
        char text[512];
        FILE *written = temporary();
        FILE *reread;
        edds_metadata metadata;
        edds_metadata parsed;
        edds_error error;
        long size;
        memset(&metadata, 0, sizeof metadata);
        memcpy(metadata.guid, "0123456789ABCDEF", 17);
        (void)snprintf(metadata.name, sizeof metadata.name, "Probe/pixel.edds");
        (void)snprintf(metadata.source_file, sizeof metadata.source_file, "pixel%s",
            capabilities[at].extension);
        metadata.source_format = capabilities[at].format;
        edds_default_profile(&metadata.profile);
        CHECK(written != NULL);
        CHECK(edds_metadata_write(written, &metadata, &error) == EDDS_OK);
        CHECK(fseek(written, 0, SEEK_END) == 0 && (size = ftell(written)) > 0);
        CHECK((size_t)size < sizeof text && fseek(written, 0, SEEK_SET) == 0);
        CHECK(fread(text, 1, (size_t)size, written) == (size_t)size);
        fclose(written);
        CHECK(strstr(text, capabilities[at].resource_class) != NULL);
        reread = stream_of((const uint8_t *)text, (size_t)size);
        CHECK(reread != NULL);
        CHECK(edds_metadata_parse(reread, &parsed, &error) == EDDS_OK);
        CHECK(parsed.source_format == capabilities[at].format);
        CHECK(strcmp(parsed.source_file, metadata.source_file) == 0);
        fclose(reread);
    }
    return 1;
}

static int metadata_source_class_must_match_its_extension(void) {
    static const char source[] =
        "MetaFileClass { Name \"{0123456789ABCDEF}a.edds\" Configurations { "
        "TGAResourceClass PC { SourceFile \"a.png\" } } }";
    FILE *input = stream_of((const uint8_t *)source, strlen(source));
    edds_metadata metadata;
    edds_error error;
    CHECK(input != NULL);
    CHECK(edds_metadata_parse(input, &metadata, &error) == EDDS_INVALID_INPUT);
    CHECK(strcmp(error.code, "source-format-mismatch") == 0);
    fclose(input);
    return 1;
}

static int batch_ndjson_parser_has_a_bounded_mutation_corpus(void) {
    static const char header[] =
        "{\"protocolVersion\":1,\"kind\":\"batch\",\"jobCount\":100}";
    static const char job[] =
        "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"a\","
        "\"input\":\"C:\\\\a.png\",\"output\":\"C:\\\\a.edds\","
        "\"metadata\":null,\"identity\":null,\"profile\":{"
        "\"TargetFormat\":\"EnfusionDDS\",\"FormatCompress\":\"Fastest\","
        "\"CompressTreshold\":80,\"Conversion\":\"None\",\"ConversionQuality\":1,"
        "\"Swizzling\":\"None\",\"GenerateMips\":true,\"MipMapFunction\":\"Filter\","
        "\"MipMapFilter\":\"Box\",\"TiledTexture\":true},\"expected\":null}";
    static const char incompatible[] =
        "{\"protocolVersion\":2,\"kind\":\"batch\",\"jobCount\":1}";
    static const char unknown[] = "{\"protocolVersion\":1,\"kind\":\"surprise\"}";
    edds_batch_record record;
    edds_error error;
    CHECK(edds_batch_parse_line(header, strlen(header), &record, &error) == EDDS_OK);
    CHECK(record.kind == EDDS_BATCH_HEADER && record.job_count == 100u);
    CHECK(edds_batch_parse_line(job, strlen(job), &record, &error) == EDDS_OK);
    CHECK(record.kind == EDDS_BATCH_JOB && strcmp(record.job.id, "a") == 0);
    for (size_t size = 0; size < strlen(job); ++size) {
        CHECK(edds_batch_parse_line(job, size, &record, &error) == EDDS_INVALID_INVOCATION);
    }
    CHECK(edds_batch_parse_line(incompatible, strlen(incompatible), &record, &error) ==
        EDDS_INVALID_INVOCATION);
    CHECK(strcmp(error.code, "incompatible-batch-version") == 0);
    CHECK(edds_batch_parse_line(unknown, strlen(unknown), &record, &error) ==
        EDDS_INVALID_INVOCATION);
    CHECK(strcmp(error.code, "unknown-batch-record") == 0);
    CHECK(edds_batch_parse_line("{}", EDDS_BATCH_MAX_LINE_BYTES + 1u, &record, &error) ==
        EDDS_INVALID_INVOCATION);
    CHECK(strcmp(error.code, "batch-line-size") == 0);
    return 1;
}

static int batch_reader_frames_lines_however_a_pipe_split_them(void) {
    static const char stream[] = "{\"a\":1}\n\r\n{\"b\":2}\r\n{\"c\":3}";
    const size_t total = sizeof stream - 1u;
    /* Every split of the same bytes has to frame the same four lines, the blank one included. */
    for (size_t chunk = 1u; chunk <= total; ++chunk) {
        edds_batch_reader reader;
        char framed[8][64];
        size_t lines = 0;
        size_t at = 0;
        edds_batch_reader_init(&reader);
        while (at < total) {
            size_t remaining = total - at < chunk ? total - at : chunk;
            const char *data = stream + at;
            at += remaining;
            while (remaining > 0u) {
                size_t consumed = 0;
                size_t size = 0;
                const edds_batch_line line =
                    edds_batch_reader_push(&reader, data, remaining, &consumed, &size);
                data += consumed;
                remaining -= consumed;
                if (line != EDDS_BATCH_LINE_READY) continue;
                CHECK(lines < 8u && size < sizeof framed[0]);
                memcpy(framed[lines++], reader.line, size + 1u);
            }
        }
        {
            size_t size = 0;
            CHECK(edds_batch_reader_finish(&reader, &size) == EDDS_BATCH_LINE_READY);
            CHECK(lines < 8u && size < sizeof framed[0]);
            memcpy(framed[lines++], reader.line, size + 1u);
        }
        CHECK(lines == 4u);
        CHECK(strcmp(framed[0], "{\"a\":1}") == 0);
        CHECK(strcmp(framed[1], "") == 0);
        CHECK(strcmp(framed[2], "{\"b\":2}") == 0);
        CHECK(strcmp(framed[3], "{\"c\":3}") == 0);
    }
    return 1;
}

static int batch_reader_refuses_an_oversized_line_without_framing_half_of_it(void) {
    const size_t span = EDDS_BATCH_MAX_LINE_BYTES + 20u;
    edds_batch_reader reader;
    char *oversized = malloc(span);
    size_t consumed = 0;
    size_t size = 0;
    edds_batch_line line;
    CHECK(oversized != NULL);
    memset(oversized, 'x', EDDS_BATCH_MAX_LINE_BYTES + 16u);
    oversized[EDDS_BATCH_MAX_LINE_BYTES + 16u] = '\n';
    memcpy(oversized + EDDS_BATCH_MAX_LINE_BYTES + 17u, "{}\n", 3u);
    edds_batch_reader_init(&reader);
    line = edds_batch_reader_push(&reader, oversized, span, &consumed, &size);
    CHECK(line == EDDS_BATCH_LINE_OVERFLOW);
    CHECK(consumed == EDDS_BATCH_MAX_LINE_BYTES + 17u);
    /* The line behind the refused one is still framed whole, not from the middle of it. */
    line = edds_batch_reader_push(&reader, oversized + consumed, span - consumed, &consumed, &size);
    CHECK(line == EDDS_BATCH_LINE_READY && size == 2u && strcmp(reader.line, "{}") == 0);
    free(oversized);
    return 1;
}

typedef struct pool_probe {
    unsigned long ran[512];
    uint32_t workers;
    uint32_t live;
    uint32_t peak;
} pool_probe;

static void pool_probe_task(void *context, uint32_t index, edds_pool *pool) {
    pool_probe *probe = (pool_probe *)context;
    const uint64_t charge = 3u * 1024u * 1024u;
    edds_pool_reserve(pool, charge);
    edds_pool_lock_output(pool);
    probe->workers = edds_pool_workers(pool);
    probe->ran[index] += 1ul;
    if (++probe->live > probe->peak) probe->peak = probe->live;
    edds_pool_unlock_output(pool);
    edds_pool_lock_output(pool);
    --probe->live;
    edds_pool_unlock_output(pool);
    edds_pool_release(pool, charge);
}

static int the_pool_runs_every_job_once_inside_one_bounded_budget(void) {
    pool_probe probe;
    edds_error error;
    memset(&probe, 0, sizeof probe);
    CHECK(edds_pool_run(256u, 8u * 1024u * 1024u, pool_probe_task, &probe, &error) == EDDS_OK);
    for (uint32_t at = 0; at < 256u; ++at) CHECK(probe.ran[at] == 1ul);
    for (uint32_t at = 256u; at < 512u; ++at) CHECK(probe.ran[at] == 0ul);
    /* Eight megabytes of budget admit two three-megabyte images at a time, never a third. */
    CHECK(probe.peak <= 2u);
    CHECK(probe.workers >= 1u && probe.workers <= EDDS_POOL_MAX_WORKERS);
    CHECK(edds_pool_worker_count(1u) == 1u);
    CHECK(edds_pool_worker_count(0u) >= 1u);
    memset(&probe, 0, sizeof probe);
    CHECK(edds_pool_run(0u, 0u, pool_probe_task, &probe, &error) == EDDS_OK);
    CHECK(probe.ran[0] == 0ul);
    return 1;
}

static int an_image_larger_than_the_whole_budget_still_runs_alone(void) {
    pool_probe probe;
    edds_error error;
    memset(&probe, 0, sizeof probe);
    /* Three megabytes charged against a budget of one: refusing it would strand the image. */
    CHECK(edds_pool_run(8u, 1024u * 1024u, pool_probe_task, &probe, &error) == EDDS_OK);
    for (uint32_t at = 0; at < 8u; ++at) CHECK(probe.ran[at] == 1ul);
    CHECK(probe.peak == 1u);
    CHECK(edds_pool_charge_of(0u) > 0u);
    CHECK(edds_pool_charge_of(1024u) > edds_pool_charge_of(0u));
    CHECK(edds_pool_charge_of(UINT64_MAX) == UINT64_MAX);
    return 1;
}

static void pool_mixed_task(void *context, uint32_t index, edds_pool *pool) {
    pool_probe *probe = (pool_probe *)context;
    /* Every fourth image needs the whole budget; the rest are small enough to share it. */
    const uint64_t charge = index % 4u == 0u ? 4u * 1024u * 1024u : 256u * 1024u;
    edds_pool_reserve(pool, charge);
    edds_pool_lock_output(pool);
    probe->ran[index] += 1ul;
    if (++probe->live > probe->peak) probe->peak = probe->live;
    edds_pool_unlock_output(pool);
    edds_pool_lock_output(pool);
    --probe->live;
    edds_pool_unlock_output(pool);
    edds_pool_release(pool, charge);
}

static int oversized_and_small_images_share_one_budget_without_starving(void) {
    pool_probe probe;
    edds_error error;
    memset(&probe, 0, sizeof probe);
    /* Images wanting everything and images wanting a little, interleaved: all of them finish. */
    CHECK(edds_pool_run(128u, 4u * 1024u * 1024u, pool_mixed_task, &probe, &error) == EDDS_OK);
    for (uint32_t at = 0; at < 128u; ++at) CHECK(probe.ran[at] == 1ul);
    return 1;
}


/*
 * The proven JPG and TIFF subtype matrix, stated as tests. Every subtype the decoders accept is
 * here with the pixels it owes, and every subtype they refuse is here with the refusal it owes.
 */

/** Converts one source through the public seam and hands back its level-zero RGBA. */
static int decoded_through_convert(
    const uint8_t *bytes,
    size_t size,
    edds_source_format format,
    int generate_mips,
    edds_info *info,
    uint8_t **rgba,
    size_t *rgba_size
) {
    FILE *source = stream_of(bytes, size);
    FILE *output = temporary();
    edds_profile profile;
    edds_error error;
    int ok;
    if (source == NULL || output == NULL) {
        if (source != NULL) fclose(source);
        if (output != NULL) fclose(output);
        return 0;
    }
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    profile.generate_mips = generate_mips;
    ok = edds_convert(source, format, output, &profile,
            never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK &&
        fseek(output, 0, SEEK_SET) == 0 &&
        edds_inspect(output, info, never_cancelled, NULL, &error) == EDDS_OK &&
        edds_preview(output, info, 0, never_cancelled, NULL, rgba, rgba_size, &error) == EDDS_OK;
    fclose(source);
    fclose(output);
    return ok;
}

/**
 * Runs a source that has to be refused. Returns the refusal, or EDDS_OK — which no refusal ever
 * is — when the conversion either succeeded or left bytes behind it.
 */
static edds_status refused_by(
    const uint8_t *bytes,
    size_t size,
    edds_source_format format,
    edds_error *error
) {
    FILE *source = stream_of(bytes, size);
    FILE *output = temporary();
    edds_profile profile;
    edds_status status;
    long written = -1;
    if (source == NULL || output == NULL) {
        if (source != NULL) fclose(source);
        if (output != NULL) fclose(output);
        return EDDS_OK;
    }
    edds_default_profile(&profile);
    status = edds_convert(source, format, output, &profile,
        never_cancelled, NULL, NULL, NULL, error);
    if (fseek(output, 0, SEEK_END) == 0) {
        written = ftell(output);
    }
    fclose(source);
    fclose(output);
    return status == EDDS_OK || written != 0 ? EDDS_OK : status;
}

static int every_pixel_is(const uint8_t *rgba, size_t size, uint8_t red, uint8_t green, uint8_t blue) {
    for (size_t at = 0; at + 3u < size; at += 4u) {
        if (rgba[at] != red || rgba[at + 1u] != green ||
            rgba[at + 2u] != blue || rgba[at + 3u] != 255u) {
            return 0;
        }
    }
    return 1;
}

static int baseline_jpeg_carries_two_flat_mcus_without_alpha(void) {
    test_bytes jpeg = fixture_jpeg_ycbcr();
    edds_info info;
    uint8_t *rgba = NULL;
    size_t rgba_size = 0;
    CHECK(jpeg.data != NULL);
    CHECK(decoded_through_convert(jpeg.data, jpeg.size, EDDS_SOURCE_JPG, 0, &info, &rgba, &rgba_size));
    CHECK(info.width == 16 && info.height == 8 && info.mip_count == 1);
    /* JPEG has no alpha to declare, so the surface is the opaque one. */
    CHECK(info.pixel_format == EDDS_PIXEL_BGRX8);
    CHECK(rgba_size == 16u * 8u * 4u);
    for (size_t pixel = 0; pixel < 16u * 8u; ++pixel) {
        const uint8_t expected = pixel % 16u < 8u ? 78u : 178u;
        const uint8_t *at = rgba + pixel * 4u;
        CHECK(at[0] == expected && at[1] == expected && at[2] == expected && at[3] == 255u);
    }
    edds_free(rgba);
    fixture_free(jpeg);
    return 1;
}

static int greyscale_jpeg_decodes_as_one_component(void) {
    static const uint8_t entropy[] = { 0x8d, 0xef };
    const fixture_jpeg_spec spec = { .frame_marker = 0xc0, .precision = 8, .width = 8, .height = 8,
          .component_count = 1, .luma_sampling = 0x11,
          .entropy = entropy, .entropy_size = sizeof entropy };
    uint8_t file[512];
    const size_t size = fixture_jpeg_build(file, sizeof file, &spec);
    edds_info info;
    uint8_t *rgba = NULL;
    size_t rgba_size = 0;
    CHECK(size != 0);
    CHECK(decoded_through_convert(file, size, EDDS_SOURCE_JPG, 0, &info, &rgba, &rgba_size));
    CHECK(info.width == 8 && info.height == 8 && info.pixel_format == EDDS_PIXEL_BGRX8);
    CHECK(rgba_size == 8u * 8u * 4u && every_pixel_is(rgba, rgba_size, 78, 78, 78));
    edds_free(rgba);
    return 1;
}

/** 4:2:0 chroma is replicated across its luma block, and the JFIF matrix places the colour. */
static int subsampled_jpeg_upsamples_chroma_over_its_luma_block(void) {
    static const uint8_t entropy[] = { 0x8d, 0xe0, 0x23, 0x78 };
    const fixture_jpeg_spec spec = { .frame_marker = 0xc0, .precision = 8, .width = 16, .height = 16,
          .component_count = 3, .luma_sampling = 0x22,
          .entropy = entropy, .entropy_size = sizeof entropy };
    uint8_t file[512];
    const size_t size = fixture_jpeg_build(file, sizeof file, &spec);
    edds_info info;
    uint8_t *rgba = NULL;
    size_t rgba_size = 0;
    CHECK(size != 0);
    CHECK(decoded_through_convert(file, size, EDDS_SOURCE_JPG, 0, &info, &rgba, &rgba_size));
    CHECK(info.width == 16 && info.height == 16);
    CHECK(rgba_size == 16u * 16u * 4u);
    CHECK(every_pixel_is(rgba, rgba_size, 78, 95, 0));
    edds_free(rgba);
    return 1;
}

static int jpeg_restart_markers_reset_the_dc_prediction(void) {
    static const uint8_t entropy[] = { 0x8d, 0xe0, 0xff, 0xd0, 0xb2, 0x00 };
    const fixture_jpeg_spec spec = { .frame_marker = 0xc0, .precision = 8, .width = 16, .height = 8,
          .component_count = 3, .luma_sampling = 0x11, .restart_interval = 1,
          .entropy = entropy, .entropy_size = sizeof entropy };
    uint8_t file[512];
    const size_t size = fixture_jpeg_build(file, sizeof file, &spec);
    edds_info info;
    uint8_t *rgba = NULL;
    size_t rgba_size = 0;
    CHECK(size != 0);
    CHECK(decoded_through_convert(file, size, EDDS_SOURCE_JPG, 0, &info, &rgba, &rgba_size));
    CHECK(rgba_size == 16u * 8u * 4u);
    for (size_t pixel = 0; pixel < 16u * 8u; ++pixel) {
        CHECK(rgba[pixel * 4u] == (pixel % 16u < 8u ? 78u : 178u));
    }
    edds_free(rgba);
    return 1;
}

static int unsupported_jpeg_subtypes_are_refused_before_any_pixel(void) {
    static const uint8_t entropy[] = { 0x8d, 0xe0, 0xf2, 0x00, 0x7f };
    static const uint8_t exif_rotated[] = {
        'E', 'x', 'i', 'f', 0, 0,
        'I', 'I', 0x2a, 0, 8, 0, 0, 0,
        1, 0,
        0x12, 0x01, 3, 0, 1, 0, 0, 0, 6, 0, 0, 0,
        0, 0, 0, 0
    };
    const struct {
        fixture_jpeg_spec spec;
        const char *code;
    } cases[] = {
        { { .frame_marker = 0xc2, .precision = 8, .width = 16, .height = 8,
            .component_count = 3, .luma_sampling = 0x11,
            .entropy = entropy, .entropy_size = sizeof entropy },
          "unsupported-jpeg-frame" },
        { { .frame_marker = 0xc9, .precision = 8, .width = 16, .height = 8,
            .component_count = 3, .luma_sampling = 0x11,
            .entropy = entropy, .entropy_size = sizeof entropy },
          "unsupported-jpeg-frame" },
        { { .frame_marker = 0xc0, .precision = 12, .width = 16, .height = 8,
            .component_count = 3, .luma_sampling = 0x11,
            .entropy = entropy, .entropy_size = sizeof entropy },
          "unsupported-jpeg-precision" },
        { { .frame_marker = 0xc0, .precision = 8, .width = 16, .height = 8,
            .component_count = 3, .luma_sampling = 0x41,
            .entropy = entropy, .entropy_size = sizeof entropy },
          "unsupported-jpeg-sampling" },
        { { .frame_marker = 0xc0, .precision = 8, .width = 8, .height = 8,
            .component_count = 1, .luma_sampling = 0x22,
            .entropy = entropy, .entropy_size = sizeof entropy },
          "unsupported-jpeg-sampling" },
        { { .frame_marker = 0xc0, .precision = 8, .width = 16, .height = 8,
            .component_count = 3, .luma_sampling = 0x11,
            .exif = exif_rotated, .exif_size = sizeof exif_rotated,
            .entropy = entropy, .entropy_size = sizeof entropy },
          "unsupported-jpeg-orientation" }
    };
    for (size_t at = 0; at < sizeof cases / sizeof cases[0]; ++at) {
        uint8_t file[512];
        const size_t size = fixture_jpeg_build(file, sizeof file, &cases[at].spec);
        edds_error error;
        memset(&error, 0, sizeof error);
        CHECK(size != 0);
        CHECK(refused_by(file, size, EDDS_SOURCE_JPG, &error) == EDDS_UNSUPPORTED_FORMAT);
        CHECK(strcmp(error.code, cases[at].code) == 0);
    }
    return 1;
}

static int damaged_jpeg_input_fails_without_partial_output(void) {
    static const uint8_t entropy[] = { 0x8d, 0xe0, 0xf2 };
    const fixture_jpeg_spec truncated =
        { .frame_marker = 0xc0, .precision = 8, .width = 16, .height = 8,
          .component_count = 3, .luma_sampling = 0x11,
          .entropy = entropy, .entropy_size = sizeof entropy, .omit_end_of_image = 1 };
    const fixture_jpeg_spec complete =
        { .frame_marker = 0xc0, .precision = 8, .width = 16, .height = 8,
          .component_count = 3, .luma_sampling = 0x11,
          .entropy = entropy, .entropy_size = sizeof entropy };
    uint8_t file[512];
    size_t size = fixture_jpeg_build(file, sizeof file, &truncated);
    edds_error error;
    memset(&error, 0, sizeof error);
    CHECK(size != 0);
    CHECK(refused_by(file, size, EDDS_SOURCE_JPG, &error) == EDDS_INVALID_INPUT);
    CHECK(strcmp(error.code, "truncated-jpeg-scan") == 0);
    /* Every prefix of a whole file is an input failure, never a crash and never half a texture. */
    size = fixture_jpeg_build(file, sizeof file, &complete);
    CHECK(size != 0);
    for (size_t prefix = 0; prefix < size; ++prefix) {
        memset(&error, 0, sizeof error);
        CHECK(refused_by(file, prefix, EDDS_SOURCE_JPG, &error) != EDDS_OK);
    }
    return 1;
}

enum { TIFF_PIXEL_BYTES = 18 };

static const uint8_t tiff_rgb_pixels[TIFF_PIXEL_BYTES] = {
    10, 20, 30, 40, 50, 60, 70, 80, 90,
    100, 110, 120, 130, 140, 150, 160, 170, 180
};

/** A legal, if wasteful, LZW stream: clear, every byte as its own code, end. */
static size_t pack_lzw_literals(uint8_t *output, size_t capacity, const uint8_t *data, size_t size) {
    const size_t codes = size + 2u;
    const size_t bytes = (codes * 9u + 7u) / 8u;
    size_t bit = 0;
    if (capacity < bytes) {
        return 0;
    }
    memset(output, 0, bytes);
    for (size_t at = 0; at < codes; ++at) {
        const uint32_t code = at == 0u ? 256u : at == codes - 1u ? 257u : data[at - 1u];
        for (uint32_t step = 0; step < 9u; ++step) {
            if (((code >> (8u - step)) & 1u) != 0u) {
                output[(bit + step) / 8u] |= (uint8_t)(1u << (7u - (bit + step) % 8u));
            }
        }
        bit += 9u;
    }
    return bytes;
}

static size_t pack_zlib_stored(uint8_t *output, size_t capacity, const uint8_t *data, size_t size) {
    uint32_t first = 1;
    uint32_t second = 0;
    if (capacity < size + 11u || size > 0xffffu) {
        return 0;
    }
    output[0] = 0x78;
    output[1] = 0x01;
    output[2] = 0x01;
    output[3] = (uint8_t)size;
    output[4] = (uint8_t)(size >> 8);
    output[5] = (uint8_t)~(uint8_t)size;
    output[6] = (uint8_t)~(uint8_t)(size >> 8);
    memcpy(output + 7, data, size);
    for (size_t at = 0; at < size; ++at) {
        first = (first + data[at]) % 65521u;
        second = (second + first) % 65521u;
    }
    output[7 + size] = (uint8_t)(second >> 8);
    output[8 + size] = (uint8_t)second;
    output[9 + size] = (uint8_t)(first >> 8);
    output[10 + size] = (uint8_t)first;
    return size + 11u;
}

/** The nine-tag baseline RGB directory every compression case here shares. */
static size_t build_rgb_tiff(
    uint8_t *output,
    size_t capacity,
    int big_endian,
    uint32_t compression,
    const uint8_t *strip,
    size_t strip_size
) {
    const uint32_t bits_at = (uint32_t)fixture_tiff_ifd_end(9);
    const uint32_t pixels_at = bits_at + 6u;
    const fixture_tiff_tag tags[9] = {
        { 256, 3, 1, 3 },
        { 257, 3, 1, 2 },
        { 258, 3, 3, bits_at },
        { 259, 3, 1, (uint16_t)compression },
        { 262, 3, 1, 2 },
        { 273, 4, 1, pixels_at },
        { 277, 3, 1, 3 },
        { 278, 3, 1, 2 },
        { 279, 4, 1, (uint32_t)strip_size }
    };
    uint8_t trailing[6 + 256];
    if (strip_size > sizeof trailing - 6u) {
        return 0;
    }
    memset(trailing, 0, sizeof trailing);
    trailing[big_endian ? 1 : 0] = 8;
    trailing[big_endian ? 3 : 2] = 8;
    trailing[big_endian ? 5 : 4] = 8;
    memcpy(trailing + 6, strip, strip_size);
    return fixture_tiff_build(output, capacity, big_endian, tags, 9, trailing, 6u + strip_size);
}

static int tiff_compressions_and_byte_orders_agree_on_one_image(void) {
    static const uint8_t expected[] = {
        10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255,
        100, 110, 120, 255, 130, 140, 150, 255, 160, 170, 180, 255
    };
    uint8_t packbits[TIFF_PIXEL_BYTES + 1];
    uint8_t lzw[64];
    uint8_t deflate[TIFF_PIXEL_BYTES + 16];
    const size_t lzw_size = pack_lzw_literals(lzw, sizeof lzw, tiff_rgb_pixels, TIFF_PIXEL_BYTES);
    const size_t deflate_size =
        pack_zlib_stored(deflate, sizeof deflate, tiff_rgb_pixels, TIFF_PIXEL_BYTES);
    struct { uint32_t compression; const uint8_t *strip; size_t size; int big_endian; } cases[] = {
        { 1, tiff_rgb_pixels, TIFF_PIXEL_BYTES, 0 },
        { 1, tiff_rgb_pixels, TIFF_PIXEL_BYTES, 1 },
        { 32773, packbits, sizeof packbits, 0 },
        { 5, lzw, 0, 0 },
        { 8, deflate, 0, 0 },
        { 32946, deflate, 0, 0 }
    };
    packbits[0] = (uint8_t)(TIFF_PIXEL_BYTES - 1u);
    memcpy(packbits + 1, tiff_rgb_pixels, TIFF_PIXEL_BYTES);
    CHECK(lzw_size != 0 && deflate_size != 0);
    cases[3].size = lzw_size;
    cases[4].size = deflate_size;
    cases[5].size = deflate_size;
    for (size_t at = 0; at < sizeof cases / sizeof cases[0]; ++at) {
        uint8_t file[512];
        edds_info info;
        uint8_t *rgba = NULL;
        size_t rgba_size = 0;
        const size_t size = build_rgb_tiff(file, sizeof file, cases[at].big_endian,
            cases[at].compression, cases[at].strip, cases[at].size);
        CHECK(size != 0);
        CHECK(decoded_through_convert(file, size, EDDS_SOURCE_TIFF, 0, &info, &rgba, &rgba_size));
        CHECK(info.width == 3 && info.height == 2 && info.pixel_format == EDDS_PIXEL_BGRX8);
        CHECK(rgba_size == sizeof expected && memcmp(rgba, expected, sizeof expected) == 0);
        edds_free(rgba);
    }
    return 1;
}

/** Alpha is a fact of the source: it arrives only where an unassociated extra sample declares it. */
static int tiff_unassociated_extra_sample_is_the_only_alpha(void) {
    static const uint8_t pixels[] = { 10, 20, 30, 40, 50, 60, 70, 80 };
    const uint32_t bits_at = (uint32_t)fixture_tiff_ifd_end(10);
    const uint32_t pixels_at = bits_at + 8u;
    fixture_tiff_tag tags[10] = {
        { 256, 3, 1, 2 },
        { 257, 3, 1, 1 },
        { 258, 3, 4, bits_at },
        { 259, 3, 1, 1 },
        { 262, 3, 1, 2 },
        { 273, 4, 1, pixels_at },
        { 277, 3, 1, 4 },
        { 278, 3, 1, 1 },
        { 279, 4, 1, (uint32_t)sizeof pixels },
        { 338, 3, 1, 2 }
    };
    uint8_t trailing[8 + sizeof pixels];
    uint8_t file[512];
    edds_info info;
    uint8_t *rgba = NULL;
    size_t rgba_size = 0;
    edds_error error;
    size_t size;
    memset(trailing, 0, sizeof trailing);
    trailing[0] = 8;
    trailing[2] = 8;
    trailing[4] = 8;
    trailing[6] = 8;
    memcpy(trailing + 8, pixels, sizeof pixels);
    size = fixture_tiff_build(file, sizeof file, 0, tags, 10, trailing, sizeof trailing);
    CHECK(size != 0);
    CHECK(decoded_through_convert(file, size, EDDS_SOURCE_TIFF, 0, &info, &rgba, &rgba_size));
    CHECK(info.pixel_format == EDDS_PIXEL_BGRA8);
    CHECK(rgba_size == sizeof pixels && memcmp(rgba, pixels, sizeof pixels) == 0);
    edds_free(rgba);

    /* Premultiplied alpha is a different sample meaning, so it is refused rather than shown. */
    tags[9].value = 1;
    size = fixture_tiff_build(file, sizeof file, 0, tags, 10, trailing, sizeof trailing);
    memset(&error, 0, sizeof error);
    CHECK(size != 0);
    CHECK(refused_by(file, size, EDDS_SOURCE_TIFF, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-tiff-alpha") == 0);
    return 1;
}

static int unsupported_tiff_subtypes_are_refused_before_any_pixel(void) {
    const struct { uint16_t tag; uint32_t value; const char *code; } changes[] = {
        { 262, 3, "unsupported-tiff-channels" },
        { 284, 2, "unsupported-tiff-layout" },
        { 317, 2, "unsupported-tiff-predictor" },
        { 274, 6, "unsupported-tiff-orientation" },
        { 322, 8, "unsupported-tiff-layout" },
        { 266, 2, "unsupported-tiff-layout" },
        { 259, 7, "unsupported-tiff-compression" },
        { 339, 3, "unsupported-tiff-sample-format" }
    };
    for (size_t at = 0; at < sizeof changes / sizeof changes[0]; ++at) {
        const uint32_t bits_at = (uint32_t)fixture_tiff_ifd_end(10);
        const uint32_t pixels_at = bits_at + 6u;
        fixture_tiff_tag tags[10] = {
            { 256, 3, 1, 3 },
            { 257, 3, 1, 2 },
            { 258, 3, 3, bits_at },
            { 259, 3, 1, 1 },
            { 262, 3, 1, 2 },
            { 273, 4, 1, pixels_at },
            { 277, 3, 1, 3 },
            { 278, 3, 1, 2 },
            { 279, 4, 1, TIFF_PIXEL_BYTES },
            { changes[at].tag, 3, 1, changes[at].value }
        };
        uint8_t trailing[6 + TIFF_PIXEL_BYTES];
        uint8_t file[512];
        edds_error error;
        size_t size;
        for (size_t entry = 0; entry < 9u; ++entry) {
            if (tags[entry].tag == changes[at].tag) {
                /* A tag the baseline already carries is overridden where it stands. */
                tags[entry].value = changes[at].value;
                tags[9] = tags[8];
            }
        }
        memset(trailing, 0, sizeof trailing);
        trailing[0] = 8;
        trailing[2] = 8;
        trailing[4] = 8;
        memcpy(trailing + 6, tiff_rgb_pixels, TIFF_PIXEL_BYTES);
        size = fixture_tiff_build(file, sizeof file, 0, tags, 10, trailing, sizeof trailing);
        memset(&error, 0, sizeof error);
        CHECK(size != 0);
        CHECK(refused_by(file, size, EDDS_SOURCE_TIFF, &error) == EDDS_UNSUPPORTED_FORMAT);
        CHECK(strcmp(error.code, changes[at].code) == 0);
    }
    return 1;
}

static int sixteen_bit_tiff_samples_are_refused(void) {
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
        { 279, 4, 1, TIFF_PIXEL_BYTES * 2u }
    };
    uint8_t trailing[6 + TIFF_PIXEL_BYTES * 2];
    uint8_t file[512];
    edds_error error;
    size_t size;
    memset(trailing, 0, sizeof trailing);
    trailing[0] = 16;
    trailing[2] = 16;
    trailing[4] = 16;
    size = fixture_tiff_build(file, sizeof file, 0, tags, 9, trailing, sizeof trailing);
    memset(&error, 0, sizeof error);
    CHECK(size != 0);
    CHECK(refused_by(file, size, EDDS_SOURCE_TIFF, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-tiff-bit-depth") == 0);
    return 1;
}

static int a_second_tiff_page_is_refused_rather_than_silently_dropped(void) {
    uint8_t file[512];
    edds_error error;
    const size_t size = build_rgb_tiff(file, sizeof file, 0, 1, tiff_rgb_pixels, TIFF_PIXEL_BYTES);
    CHECK(size != 0);
    /* The next-directory pointer sits immediately after the nine entries. */
    file[8u + 2u + 9u * 12u] = 0x80;
    memset(&error, 0, sizeof error);
    CHECK(refused_by(file, size, EDDS_SOURCE_TIFF, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-tiff-pages") == 0);
    return 1;
}

static int damaged_tiff_input_fails_without_partial_output(void) {
    uint8_t file[512];
    const size_t size = build_rgb_tiff(file, sizeof file, 0, 1, tiff_rgb_pixels, TIFF_PIXEL_BYTES);
    CHECK(size != 0);
    for (size_t prefix = 0; prefix < size; ++prefix) {
        edds_error error;
        memset(&error, 0, sizeof error);
        CHECK(refused_by(file, prefix, EDDS_SOURCE_TIFF, &error) != EDDS_OK);
    }
    return 1;
}

/** One table every part of the converter reads, rather than four remembered lists of its own. */
static int the_source_contract_names_only_registered_resource_classes(void) {
    size_t count = 0;
    const edds_source_capability *capabilities = edds_source_capabilities(&count);
    CHECK(capabilities != NULL && count == 4u);
    for (size_t at = 0; at < count; ++at) {
        CHECK(edds_source_capability_of_format(capabilities[at].format) == &capabilities[at]);
        CHECK(edds_source_capability_of_resource_class(capabilities[at].resource_class) ==
            &capabilities[at]);
    }
    /* An alias is not a resource class, however much it looks like one. */
    CHECK(edds_source_capability_of_resource_class("JPEGResourceClass") == NULL);
    CHECK(edds_source_capability_of_resource_class("TIFResourceClass") == NULL);
    CHECK(edds_source_capability_of_resource_class(NULL) == NULL);
    CHECK(edds_source_capability_of_format((edds_source_format)99) == NULL);
    return 1;
}
int main(void) {
    const int passed =
        copy_inspection_and_preview() &&
        lz4_preview_and_bgrx_alpha() &&
        lz4_stream_keeps_the_previous_dictionary() &&
        dx10_bgrx_is_a_single_surface_preview() &&
        unsupported_pixels_keep_the_inspection() &&
        contradictory_legacy_flags_never_produce_pixels() &&
        truncation_and_limits_fail_without_output() &&
        malformed_lz4_and_cancellation_are_bounded() &&
        malformed_sizes_and_oversized_files_are_refused() &&
        integer_boundaries_are_refused_before_arithmetic() &&
        supported_workbench_defaults_are_explicit() &&
        uncompressed_tga_converts_through_the_public_edds_seam() &&
        rgba_png_converts_with_declared_alpha() &&
        png_gamma_is_metadata_not_a_sample_transform() &&
        tga_origin_channels_and_declared_alpha_are_normalized() &&
        format_compress_selects_lossless_lz4_only_at_the_threshold() &&
        compression_efforts_are_observably_distinct_and_lossless() &&
        metadata_round_trip_is_canonical_and_preserves_identity() &&
        known_but_unsupported_metadata_is_never_defaulted() &&
        metadata_source_class_must_match_its_extension() &&
        every_resource_class_round_trips_through_metadata() &&
        batch_ndjson_parser_has_a_bounded_mutation_corpus() &&
        batch_reader_frames_lines_however_a_pipe_split_them() &&
        batch_reader_refuses_an_oversized_line_without_framing_half_of_it() &&
        the_pool_runs_every_job_once_inside_one_bounded_budget() &&
        an_image_larger_than_the_whole_budget_still_runs_alone() &&
        oversized_and_small_images_share_one_budget_without_starving() &&
        the_source_contract_names_only_registered_resource_classes() &&
        baseline_jpeg_carries_two_flat_mcus_without_alpha() &&
        greyscale_jpeg_decodes_as_one_component() &&
        subsampled_jpeg_upsamples_chroma_over_its_luma_block() &&
        jpeg_restart_markers_reset_the_dc_prediction() &&
        unsupported_jpeg_subtypes_are_refused_before_any_pixel() &&
        damaged_jpeg_input_fails_without_partial_output() &&
        tiff_compressions_and_byte_orders_agree_on_one_image() &&
        tiff_unassociated_extra_sample_is_the_only_alpha() &&
        unsupported_tiff_subtypes_are_refused_before_any_pixel() &&
        sixteen_bit_tiff_samples_are_refused() &&
        a_second_tiff_page_is_refused_rather_than_silently_dropped() &&
        damaged_tiff_input_fails_without_partial_output();

    return passed ? 0 : 1;
}
