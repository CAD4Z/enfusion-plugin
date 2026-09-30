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
    test_bytes fixture = fixture_odd_fourcc();
    FILE *file = stream_of(fixture.data, fixture.size);
    edds_info info;
    edds_error error;
    uint8_t *rgba = NULL;
    size_t size = 0;

    CHECK(fixture.data != NULL && file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.pixel_format == EDDS_PIXEL_UNKNOWN && info.mips[0].stored_bytes == 8);
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
    static const uint8_t expected_mip_one[] = { 85, 95, 105, 255 };
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
    CHECK(profile.remove_mips == 0u);
    CHECK(profile.contains_mips == 0);
    CHECK(profile.generate_mips == 1);
    CHECK(profile.normalize == 0);
    CHECK(profile.mipmap_function == EDDS_MIPMAP_FILTER);
    CHECK(profile.mipmap_filter == EDDS_FILTER_BOX);
    CHECK(profile.tiled_texture == 1);
    return 1;
}

static int mip_profile_refuses_conflicts_and_unproven_values(void) {
    edds_profile profile;
    edds_error error;

    edds_default_profile(&profile);
    profile.contains_mips = 1;
    CHECK(edds_profile_check(&profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-combination") == 0);

    edds_default_profile(&profile);
    profile.remove_mips = 15;
    CHECK(edds_profile_check(&profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-setting") == 0);

    edds_default_profile(&profile);
    profile.mipmap_function = EDDS_MIPMAP_COLOR_NOISE;
    profile.mipmap_filter = EDDS_FILTER_KAISER;
    CHECK(edds_profile_check(&profile, &error) == EDDS_OK);

    edds_default_profile(&profile);
    profile.mipmap_filter = EDDS_FILTER_TRIANGLE;
    CHECK(edds_profile_check(&profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strstr(error.message, "Triangle") != NULL);

    edds_default_profile(&profile);
    profile.generate_mips = 0;
    profile.mipmap_function = EDDS_MIPMAP_NORMALIZE;
    CHECK(edds_profile_check(&profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-combination") == 0);

    edds_default_profile(&profile);
    profile.mipmap_function = EDDS_MIPMAP_NORMALIZE;
    profile.mipmap_filter = EDDS_FILTER_KAISER;
    CHECK(edds_profile_check(&profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-combination") == 0);

    edds_default_profile(&profile);
    profile.tiled_texture = 0;
    CHECK(edds_profile_check(&profile, &error) == EDDS_OK);
    profile.tiled_texture = 2;
    CHECK(edds_profile_check(&profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    return 1;
}

static int dds_supplied_levels_are_removed_from_the_large_end(void) {
    static const uint8_t expected_middle[] = {
        11, 22, 33, 255, 44, 55, 66, 255
    };
    static const uint8_t expected_last[] = { 77, 88, 99, 255 };
    test_bytes dds = fixture_dds_bgrx_mips();
    FILE *source = stream_of(dds.data, dds.size);
    FILE *output = temporary();
    edds_profile profile;
    edds_info info;
    edds_error error;
    uint8_t *rgba = NULL;
    size_t rgba_size = 0;

    CHECK(dds.data != NULL && source != NULL && output != NULL);
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    profile.contains_mips = 1;
    profile.generate_mips = 0;
    profile.remove_mips = 1;
    CHECK(edds_convert(source, EDDS_SOURCE_DDS, output, &profile,
        never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.width == 2 && info.height == 1 && info.mip_count == 2);
    CHECK(edds_preview(output, &info, 0, never_cancelled, NULL,
        &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == sizeof expected_middle &&
        memcmp(rgba, expected_middle, sizeof expected_middle) == 0);
    edds_free(rgba);
    rgba = NULL;
    CHECK(edds_preview(output, &info, 1, never_cancelled, NULL,
        &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == sizeof expected_last &&
        memcmp(rgba, expected_last, sizeof expected_last) == 0);

    edds_free(rgba);
    fclose(source);
    fclose(output);
    fixture_free(dds);
    return 1;
}

static int rgba_png_converts_with_declared_alpha(void) {
    static const uint8_t expected_mip_zero[] = {
        10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120,
        110, 120, 130, 140, 150, 160, 170, 180, 190, 200, 210, 220
    };
    static const uint8_t expected_mip_one[] = { 100, 110, 120, 130 };
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
        "   RemoveMips 2\n"
        "   Conversion None\n"
        "   ConversionQuality 1\n"
        "   Swizzling None\n"
        "   ContainsMips 0\n"
        "   GenerateMips 0\n"
        "   Normalize 1\n"
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
        "   RemoveMips 2\n"
        "   Conversion None\n"
        "   ConversionQuality 1\n"
        "   Swizzling None\n"
        "   ContainsMips 0\n"
        "   GenerateMips 0\n"
        "   Normalize 1\n"
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
    CHECK(metadata.profile.compress_threshold == 73 && metadata.profile.remove_mips == 2u);
    CHECK(!metadata.profile.contains_mips && !metadata.profile.generate_mips);
    CHECK(metadata.profile.normalize && metadata.profile.mipmap_filter == EDDS_FILTER_BOX);
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
    static const struct {
        const char *setting;
        const char *code;
    } cases[] = {
        { "Conversion HDRCompression", "unsupported-setting" },
        { "MipMapFilter Triangle", "unsupported-setting" },
        { "GenerateMips 0 MipMapFunction ColorNoise", "unsupported-combination" },
        { "GenerateMips 0 MipMapFunction Normalize", "unsupported-combination" },
        { "MipMapFunction Normalize MipMapFilter Kaiser", "unsupported-combination" },
        { "ContainsMips 1 GenerateMips 1", "unsupported-combination" }
    };
    for (size_t at = 0; at < sizeof cases / sizeof cases[0]; ++at) {
        char source[512];
        edds_metadata metadata;
        edds_error error;
        FILE *input;
        (void)snprintf(source, sizeof source,
            "MetaFileClass { Name \"{0123456789ABCDEF}a.edds\" Configurations { "
            "TGAResourceClass PC { SourceFile \"a.tga\" %s } } }", cases[at].setting);
        input = stream_of((const uint8_t *)source, strlen(source));
        CHECK(input != NULL);
        CHECK(edds_metadata_parse(input, &metadata, &error) == EDDS_UNSUPPORTED_FORMAT);
        CHECK(strcmp(error.code, cases[at].code) == 0);
        fclose(input);
    }
    return 1;
}

/** Every registered resource class parses back to its own format, and only to its own. */
static int every_resource_class_round_trips_through_metadata(void) {
    size_t count = 0;
    const edds_source_capability *capabilities = edds_source_capabilities(&count);
    CHECK(count == 5u);
    for (size_t at = 0; at < count; ++at) {
        char text[768];
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
        if (metadata.source_format == EDDS_SOURCE_DDS) {
            metadata.profile.contains_mips = 1;
            metadata.profile.generate_mips = 0;
        }
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
        "\"CompressTreshold\":80,\"RemoveMips\":2,\"Conversion\":\"None\","
        "\"ConversionQuality\":1,\"Swizzling\":\"None\",\"ContainsMips\":false,"
        "\"GenerateMips\":true,\"Normalize\":true,\"MipMapFunction\":\"Normalize\","
        "\"MipMapFilter\":\"Box\",\"TiledTexture\":true},\"expected\":null}";
    static const char inactive_mip_setting[] =
        "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"a\","
        "\"input\":\"C:\\\\a.png\",\"output\":\"C:\\\\a.edds\","
        "\"metadata\":null,\"identity\":null,\"profile\":{"
        "\"TargetFormat\":\"EnfusionDDS\",\"FormatCompress\":\"Fastest\","
        "\"CompressTreshold\":80,\"RemoveMips\":0,\"Conversion\":\"None\","
        "\"ConversionQuality\":1,\"Swizzling\":\"None\",\"ContainsMips\":false,"
        "\"GenerateMips\":false,\"Normalize\":false,\"MipMapFunction\":\"Filter\","
        "\"MipMapFilter\":\"Kaiser\",\"TiledTexture\":true},\"expected\":null}";
    static const char incompatible[] =
        "{\"protocolVersion\":2,\"kind\":\"batch\",\"jobCount\":1}";
    static const char unknown[] = "{\"protocolVersion\":1,\"kind\":\"surprise\"}";
    edds_batch_record record;
    edds_error error;
    CHECK(edds_batch_parse_line(header, strlen(header), &record, &error) == EDDS_OK);
    CHECK(record.kind == EDDS_BATCH_HEADER && record.job_count == 100u);
    CHECK(edds_batch_parse_line(job, strlen(job), &record, &error) == EDDS_OK);
    CHECK(record.kind == EDDS_BATCH_JOB && strcmp(record.job.id, "a") == 0);
    CHECK(record.job.profile.remove_mips == 2u && record.job.profile.normalize);
    CHECK(record.job.profile.mipmap_function == EDDS_MIPMAP_NORMALIZE);
    CHECK(record.job.profile.mipmap_filter == EDDS_FILTER_BOX);
    CHECK(edds_batch_parse_line(inactive_mip_setting, strlen(inactive_mip_setting),
        &record, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-combination") == 0);
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
static edds_status refused_by_profile(
    const uint8_t *bytes,
    size_t size,
    edds_source_format format,
    const edds_profile *profile,
    edds_error *error
) {
    FILE *source = stream_of(bytes, size);
    FILE *output = temporary();
    edds_status status;
    long written = -1;
    if (source == NULL || output == NULL) {
        if (source != NULL) fclose(source);
        if (output != NULL) fclose(output);
        return EDDS_OK;
    }
    status = edds_convert(source, format, output, profile,
        never_cancelled, NULL, NULL, NULL, error);
    if (fseek(output, 0, SEEK_END) == 0) {
        written = ftell(output);
    }
    fclose(source);
    fclose(output);
    return status == EDDS_OK || written != 0 ? EDDS_OK : status;
}

static edds_status refused_by(
    const uint8_t *bytes,
    size_t size,
    edds_source_format format,
    edds_error *error
) {
    edds_profile profile;
    edds_default_profile(&profile);
    return refused_by_profile(bytes, size, format, &profile, error);
}

static int dds_top_level_generates_but_cannot_claim_a_supplied_chain(void) {
    test_bytes dds = fixture_dds_bgrx_top();
    FILE *source = stream_of(dds.data, dds.size);
    FILE *output = temporary();
    edds_profile profile;
    edds_info info;
    edds_error error;

    CHECK(dds.data != NULL && source != NULL && output != NULL);
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    CHECK(edds_convert(source, EDDS_SOURCE_DDS, output, &profile,
        never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.width == 4u && info.height == 2u && info.mip_count == 3u);
    fclose(source);
    fclose(output);

    profile.generate_mips = 0;
    profile.contains_mips = 1;
    CHECK(refused_by_profile(dds.data, dds.size, EDDS_SOURCE_DDS, &profile, &error) ==
        EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-dds-mip-layout") == 0);

    /* DDS permits zero for a top-only file; keep that admitted header shape controlled too. */
    memset(dds.data + 28, 0, 4);
    {
        uint8_t *rgba = NULL;
        size_t rgba_size = 0;
        CHECK(decoded_through_convert(dds.data, dds.size, EDDS_SOURCE_DDS, 0,
            &info, &rgba, &rgba_size));
        CHECK(rgba_size == 4u * 2u * 4u && rgba[0] == 1u && rgba[1] == 2u &&
            rgba[2] == 3u && rgba[3] == 255u);
        edds_free(rgba);
    }
    fixture_free(dds);
    return 1;
}

static int dds_refusals_are_precise_and_atomic(void) {
    test_bytes dds = fixture_dds_bgrx_mips();
    test_bytes top = fixture_dds_bgrx_top();
    test_bytes png = fixture_png_rgba();
    edds_profile profile;
    edds_error error;

    CHECK(dds.data != NULL && top.data != NULL && png.data != NULL && dds.size > 128u);

    edds_default_profile(&profile);
    profile.contains_mips = 1;
    memset(&error, 0, sizeof error);
    CHECK(refused_by_profile(dds.data, dds.size, EDDS_SOURCE_DDS, &profile, &error) ==
        EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-combination") == 0);

    profile.generate_mips = 0;
    profile.contains_mips = 1;
    profile.remove_mips = 3;
    memset(&error, 0, sizeof error);
    CHECK(refused_by_profile(dds.data, dds.size, EDDS_SOURCE_DDS, &profile, &error) ==
        EDDS_INVALID_INPUT);
    CHECK(strcmp(error.code, "remove-mips-out-of-range") == 0);

    profile.remove_mips = 0;
    memset(&error, 0, sizeof error);
    CHECK(refused_by_profile(dds.data, dds.size - 1u, EDDS_SOURCE_DDS, &profile, &error) ==
        EDDS_INVALID_INPUT);
    CHECK(strcmp(error.code, "truncated-dds-mip") == 0);

    for (size_t prefix = 0; prefix < dds.size; ++prefix) {
        memset(&error, 0, sizeof error);
        CHECK(refused_by_profile(dds.data, prefix, EDDS_SOURCE_DDS, &profile, &error) != EDDS_OK);
    }

    dds.data[28] = 2u;
    memset(&error, 0, sizeof error);
    CHECK(refused_by_profile(dds.data, dds.size, EDDS_SOURCE_DDS, &profile, &error) ==
        EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-dds-mip-layout") == 0);
    dds.data[28] = 3u;

    dds.data[32] = 1u;
    memset(&error, 0, sizeof error);
    CHECK(refused_by_profile(dds.data, dds.size, EDDS_SOURCE_DDS, &profile, &error) ==
        EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-dds-header") == 0);
    dds.data[32] = 0u;

    dds.data[20] += 1u;
    memset(&error, 0, sizeof error);
    CHECK(refused_by_profile(dds.data, dds.size, EDDS_SOURCE_DDS, &profile, &error) ==
        EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-dds-pitch") == 0);
    dds.data[20] -= 1u;

    ++top.size;
    profile.contains_mips = 0;
    memset(&error, 0, sizeof error);
    CHECK(refused_by_profile(top.data, top.size, EDDS_SOURCE_DDS, &profile, &error) ==
        EDDS_INVALID_INPUT);
    CHECK(strcmp(error.code, "trailing-dds-data") == 0);

    profile.contains_mips = 1;
    memset(&error, 0, sizeof error);
    CHECK(refused_by_profile(png.data, png.size, EDDS_SOURCE_PNG, &profile, &error) ==
        EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-combination") == 0);

    fixture_free(png);
    fixture_free(top);
    fixture_free(dds);
    return 1;
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
    CHECK(capabilities != NULL && count == 5u);
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

/*
 * The GPU conversions. Each one is checked against the header facts DayZ's own textures carry,
 * against pixels decoded back out of the result, and against the rule that decides its alpha
 * branch. The sources are built here rather than recorded, so what every test is about is visible.
 */

enum {
    GRADIENT_WIDTH = 9,
    GRADIENT_HEIGHT = 5,
    GRADIENT_PIXELS = GRADIENT_WIDTH * GRADIENT_HEIGHT
};

typedef enum source_alpha_shape {
    SOURCE_NO_ALPHA_CHANNEL,
    SOURCE_FULLY_OPAQUE_ALPHA,
    SOURCE_ONE_SAMPLE_BELOW_OPAQUE,
    SOURCE_ALPHA_RAMP
} source_alpha_shape;

/** A colour ramp wide enough for three block columns, with one bright sample off the ramp. */
static void gradient_bgra(uint8_t bgra[GRADIENT_PIXELS * 4], source_alpha_shape alpha) {
    for (uint32_t y = 0; y < GRADIENT_HEIGHT; ++y) {
        for (uint32_t x = 0; x < GRADIENT_WIDTH; ++x) {
            uint8_t *pixel = bgra + ((size_t)y * GRADIENT_WIDTH + x) * 4u;
            const uint8_t red = (uint8_t)((x * 255u) / (GRADIENT_WIDTH - 1u));
            const uint8_t green = (uint8_t)((y * 255u) / (GRADIENT_HEIGHT - 1u));
            pixel[0] = (uint8_t)(255u - red);
            pixel[1] = green;
            pixel[2] = red;
            pixel[3] = 255u;
            if (alpha == SOURCE_ALPHA_RAMP) {
                pixel[3] = (uint8_t)(((x + y) * 255u) / (GRADIENT_WIDTH + GRADIENT_HEIGHT - 2u));
            }
        }
    }
    if (alpha == SOURCE_ONE_SAMPLE_BELOW_OPAQUE) {
        /* One sample one step off opaque is still alpha this result has to carry. */
        bgra[(2u * GRADIENT_WIDTH + 4u) * 4u + 3u] = 254u;
    }
}

enum { SMOOTH_SIDE = 16, SMOOTH_PIXELS = SMOOTH_SIDE * SMOOTH_SIDE };

/**
 * A gentle blend, which is what a texture usually is. The error bounds are claimed against this
 * rather than against the ramp above: a ramp that moves a quarter of the range inside one block
 * measures how a single line through colour space fails, not how well the encoder finds it.
 */
static void smooth_bgra(uint8_t bgra[SMOOTH_PIXELS * 4]) {
    for (uint32_t y = 0; y < SMOOTH_SIDE; ++y) {
        for (uint32_t x = 0; x < SMOOTH_SIDE; ++x) {
            uint8_t *pixel = bgra + ((size_t)y * SMOOTH_SIDE + x) * 4u;
            pixel[0] = (uint8_t)(180u - x * 3u);
            pixel[1] = (uint8_t)(60u + y * 5u);
            pixel[2] = (uint8_t)(40u + x * 6u);
            pixel[3] = (uint8_t)(255u - y * 4u);
        }
    }
}

/** Four flat quadrants, so the same block repeats and the container has something to compress. */
static void quadrant_bgra(uint8_t bgra[SMOOTH_PIXELS * 4]) {
    for (uint32_t y = 0; y < SMOOTH_SIDE; ++y) {
        for (uint32_t x = 0; x < SMOOTH_SIDE; ++x) {
            uint8_t *pixel = bgra + ((size_t)y * SMOOTH_SIDE + x) * 4u;
            const unsigned quadrant = (x < SMOOTH_SIDE / 2u ? 0u : 1u) +
                (y < SMOOTH_SIDE / 2u ? 0u : 2u);
            pixel[0] = (uint8_t)(30u + quadrant * 60u);
            pixel[1] = (uint8_t)(200u - quadrant * 50u);
            pixel[2] = (uint8_t)(80u + quadrant * 40u);
            pixel[3] = (uint8_t)(255u - quadrant * 30u);
        }
    }
}

/** Converts a synthetic TGA and hands back the inspection plus one decoded mip. */
static int converted_source(
    const uint8_t *bgra,
    uint32_t width,
    uint32_t height,
    int with_alpha,
    const edds_profile *profile,
    uint32_t level,
    edds_info *info,
    uint8_t **rgba,
    size_t *rgba_size
) {
    uint8_t tga[18u + 64u * 64u * 4u];
    const size_t size = fixture_tga_build(tga, sizeof tga, width, height, with_alpha, bgra);
    FILE *source = size == 0 ? NULL : stream_of(tga, size);
    FILE *output = temporary();
    edds_error error;
    int ok;
    if (source == NULL || output == NULL) {
        if (source != NULL) fclose(source);
        if (output != NULL) fclose(output);
        return 0;
    }
    ok = edds_convert(source, EDDS_SOURCE_TGA, output, profile,
            never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK &&
        fseek(output, 0, SEEK_SET) == 0 &&
        edds_inspect(output, info, never_cancelled, NULL, &error) == EDDS_OK &&
        (rgba == NULL ||
            edds_preview(output, info, level, never_cancelled, NULL, rgba, rgba_size, &error) == EDDS_OK);
    fclose(source);
    fclose(output);
    return ok;
}

static int box_and_kaiser_mips_cover_npot_edges_exactly(void) {
    static const uint8_t expected_box[] = { 10, 0, 0, 255, 13, 0, 0, 255 };
    static const uint8_t expected_kaiser[] = { 0, 0, 0, 255, 0, 0, 0, 255 };
    uint8_t bgra[5u * 3u * 4u] = { 0 };
    edds_profile profile;
    edds_info info;
    uint8_t *rgba = NULL;
    size_t rgba_size = 0;

    for (uint32_t y = 0; y < 3u; ++y) {
        for (uint32_t x = 0; x < 5u; ++x) {
            const size_t at = ((size_t)y * 5u + x) * 4u;
            bgra[at + 2u] = (uint8_t)(y * 10u + x);
            bgra[at + 3u] = 255u;
        }
    }
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    CHECK(converted_source(bgra, 5, 3, 1, &profile, 1, &info, &rgba, &rgba_size));
    CHECK(info.mip_count == 3u && rgba_size == sizeof expected_box);
    CHECK(memcmp(rgba, expected_box, sizeof expected_box) == 0);
    edds_free(rgba);
    rgba = NULL;

    memset(bgra, 0, sizeof bgra);
    for (size_t at = 0; at < 15u; ++at) bgra[at * 4u + 3u] = 255u;
    bgra[(1u * 5u + 2u) * 4u + 2u] = 255u;
    profile.mipmap_filter = EDDS_FILTER_KAISER;
    CHECK(converted_source(bgra, 5, 3, 1, &profile, 1, &info, &rgba, &rgba_size));
    CHECK(rgba_size == sizeof expected_kaiser);
    CHECK(memcmp(rgba, expected_kaiser, sizeof expected_kaiser) == 0);
    edds_free(rgba);

    {
        static const uint8_t expected_line[] = { 8, 0, 0, 255, 32, 0, 0, 255 };
        uint8_t horizontal[5u * 4u] = { 0 };
        uint8_t vertical[5u * 4u] = { 0 };
        profile.mipmap_filter = EDDS_FILTER_BOX;
        for (size_t at = 0; at < 5u; ++at) {
            horizontal[at * 4u + 2u] = (uint8_t)(at * 10u);
            horizontal[at * 4u + 3u] = 255u;
            vertical[at * 4u + 2u] = (uint8_t)(at * 10u);
            vertical[at * 4u + 3u] = 255u;
        }
        rgba = NULL;
        CHECK(converted_source(horizontal, 5, 1, 1, &profile, 1, &info, &rgba, &rgba_size));
        CHECK(info.mips[1].width == 2u && info.mips[1].height == 1u);
        CHECK(rgba_size == sizeof expected_line &&
            memcmp(rgba, expected_line, sizeof expected_line) == 0);
        edds_free(rgba);
        rgba = NULL;
        CHECK(converted_source(vertical, 1, 5, 1, &profile, 1, &info, &rgba, &rgba_size));
        CHECK(info.mips[1].width == 1u && info.mips[1].height == 2u);
        CHECK(rgba_size == sizeof expected_line &&
            memcmp(rgba, expected_line, sizeof expected_line) == 0);
        edds_free(rgba);
    }
    return 1;
}

static int dds_source_matrix_covers_legacy_blocks_and_dx10_supplied_levels(void) {
    test_bytes dxt1 = fixture_dds_dxt1_top();
    test_bytes dxt5 = fixture_dds_dxt5_top();
    test_bytes r8 = fixture_dds_dx10_r8_mips();
    edds_profile profile;
    edds_info info;
    edds_error error;
    uint8_t *rgba = NULL;
    size_t rgba_size = 0;
    FILE *source;
    FILE *output;

    CHECK(dxt1.data != NULL && dxt5.data != NULL && r8.data != NULL);
    CHECK(decoded_through_convert(dxt1.data, dxt1.size, EDDS_SOURCE_DDS, 0,
        &info, &rgba, &rgba_size));
    CHECK(info.width == 4u && info.height == 4u && info.mip_count == 1u);
    CHECK(rgba_size == 4u * 4u * 4u && every_pixel_is(rgba, rgba_size, 0, 0, 0));
    edds_free(rgba);
    rgba = NULL;

    CHECK(decoded_through_convert(dxt5.data, dxt5.size, EDDS_SOURCE_DDS, 0,
        &info, &rgba, &rgba_size));
    CHECK(info.width == 4u && info.height == 4u && info.mip_count == 1u);
    CHECK(rgba_size == 4u * 4u * 4u && rgba[0] == 0u && rgba[1] == 0u &&
        rgba[2] == 0u && rgba[3] == 0u);
    edds_free(rgba);
    rgba = NULL;

    {
        static const uint8_t r[] = { 17 };
        static const uint8_t rg[] = { 17, 33 };
        static const uint8_t zero8[8] = { 0 };
        static const uint8_t zero16[16] = { 0 };
        static const uint8_t bgra[] = { 3, 2, 1, 4 };
        static const uint8_t bc7[] = {
            0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x81, 0x40,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        static const struct {
            uint32_t dxgi;
            const uint8_t *sample;
            size_t sample_size;
            uint32_t bytes_per_pixel;
            uint8_t red;
            uint8_t green;
            uint8_t blue;
            uint8_t alpha;
        } cases[] = {
            { 61, r, sizeof r, 1, 17, 0, 0, 255 },
            { 49, rg, sizeof rg, 2, 17, 33, 0, 255 },
            { 71, zero8, sizeof zero8, 0, 0, 0, 0, 255 },
            { 77, zero16, sizeof zero16, 0, 0, 0, 0, 0 },
            { 80, zero8, sizeof zero8, 0, 0, 0, 0, 255 },
            { 83, zero16, sizeof zero16, 0, 0, 0, 0, 255 },
            { 87, bgra, sizeof bgra, 4, 1, 2, 3, 4 },
            { 88, bgra, sizeof bgra, 4, 1, 2, 3, 255 },
            { 98, bc7, sizeof bc7, 0, 128, 128, 128, 128 }
        };
        for (size_t at = 0; at < sizeof cases / sizeof cases[0]; ++at) {
            const size_t pixels = cases[at].bytes_per_pixel == 0u ? 1u : 16u;
            const size_t payload_size = cases[at].sample_size * pixels;
            uint8_t payload[64] = { 0 };
            test_bytes fixture;
            for (size_t pixel = 0; pixel < pixels; ++pixel) {
                memcpy(payload + pixel * cases[at].sample_size,
                    cases[at].sample, cases[at].sample_size);
            }
            fixture = fixture_dds_dx10_top(cases[at].dxgi, payload, payload_size,
                cases[at].bytes_per_pixel);
            CHECK(fixture.data != NULL);
            CHECK(decoded_through_convert(fixture.data, fixture.size, EDDS_SOURCE_DDS, 0,
                &info, &rgba, &rgba_size));
            CHECK(info.width == 4u && info.height == 4u && rgba_size == 4u * 4u * 4u);
            CHECK(rgba[0] == cases[at].red && rgba[1] == cases[at].green &&
                rgba[2] == cases[at].blue && rgba[3] == cases[at].alpha);
            edds_free(rgba);
            rgba = NULL;
            fixture_free(fixture);
        }
    }

    source = stream_of(r8.data, r8.size);
    output = temporary();
    CHECK(source != NULL && output != NULL);
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    profile.conversion = EDDS_CONVERSION_RED;
    profile.contains_mips = 1;
    profile.generate_mips = 0;
    CHECK(edds_convert(source, EDDS_SOURCE_DDS, output, &profile,
        never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.width == 4u && info.height == 2u && info.mip_count == 3u &&
        info.pixel_format == EDDS_PIXEL_R8);
    for (uint32_t level = 0; level < info.mip_count; ++level) {
        static const uint8_t expected[] = { 1, 11, 33 };
        CHECK(edds_preview(output, &info, level, never_cancelled, NULL,
            &rgba, &rgba_size, &error) == EDDS_OK);
        CHECK(rgba_size == (size_t)info.mips[level].width * info.mips[level].height * 4u);
        CHECK(rgba[0] == expected[level] && rgba[1] == 0u &&
            rgba[2] == 0u && rgba[3] == 255u);
        edds_free(rgba);
        rgba = NULL;
    }
    fclose(source);
    fclose(output);
    fixture_free(dxt1);
    fixture_free(dxt5);
    fixture_free(r8);
    return 1;
}

static int dds_alpha_branch_scans_every_supplied_level(void) {
    test_bytes dds = fixture_dds_bgra_alpha_mips();
    FILE *source = stream_of(dds.data, dds.size);
    FILE *output = temporary();
    edds_profile profile;
    edds_info info;
    edds_error error;
    uint8_t *rgba = NULL;
    size_t rgba_size = 0;

    CHECK(dds.data != NULL && source != NULL && output != NULL);
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    profile.conversion = EDDS_CONVERSION_DXT;
    profile.contains_mips = 1;
    profile.generate_mips = 0;
    CHECK(edds_convert(source, EDDS_SOURCE_DDS, output, &profile,
        never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.pixel_format == EDDS_PIXEL_DXT5 && info.mip_count == 3u);
    CHECK(edds_preview(output, &info, 2, never_cancelled, NULL,
        &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == 4u && rgba[3] < 255u);

    edds_free(rgba);
    fclose(source);
    fclose(output);
    fixture_free(dds);
    return 1;
}

static int normalize_flag_and_mipmap_function_have_distinct_stages(void) {
    static const uint8_t source_bgra[] = {
        127, 127, 191, 33,
        127, 191, 127, 77
    };
    static const uint8_t expected_normalized_source[] = {
        255, 128, 128, 33,
        128, 255, 128, 77
    };
    static const uint8_t expected_pre_normalized_mip[] = { 192, 192, 128, 55 };
    static const uint8_t expected_post_normalized_mip[] = { 218, 218, 128, 55 };
    edds_profile profile;
    edds_info info;
    uint8_t *rgba = NULL;
    size_t rgba_size = 0;

    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    profile.normalize = 1;
    CHECK(converted_source(source_bgra, 2, 1, 1, &profile, 0,
        &info, &rgba, &rgba_size));
    CHECK(rgba_size == sizeof expected_normalized_source &&
        memcmp(rgba, expected_normalized_source, sizeof expected_normalized_source) == 0);
    edds_free(rgba);
    rgba = NULL;
    CHECK(converted_source(source_bgra, 2, 1, 1, &profile, 1,
        &info, &rgba, &rgba_size));
    CHECK(rgba_size == sizeof expected_pre_normalized_mip &&
        memcmp(rgba, expected_pre_normalized_mip, sizeof expected_pre_normalized_mip) == 0);
    edds_free(rgba);
    rgba = NULL;

    profile.normalize = 0;
    profile.mipmap_function = EDDS_MIPMAP_NORMALIZE;
    CHECK(converted_source(source_bgra, 2, 1, 1, &profile, 1,
        &info, &rgba, &rgba_size));
    CHECK(rgba_size == sizeof expected_post_normalized_mip &&
        memcmp(rgba, expected_post_normalized_mip, sizeof expected_post_normalized_mip) == 0);
    edds_free(rgba);
    return 1;
}

static edds_status refused_profile(const edds_profile *profile, edds_error *error) {
    uint8_t bgra[GRADIENT_PIXELS * 4];
    uint8_t tga[18u + GRADIENT_PIXELS * 4u];
    FILE *source;
    FILE *output = temporary();
    edds_status status;
    gradient_bgra(bgra, SOURCE_ALPHA_RAMP);
    source = stream_of(tga,
        fixture_tga_build(tga, sizeof tga, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, bgra));
    if (source == NULL || output == NULL) {
        if (source != NULL) fclose(source);
        if (output != NULL) fclose(output);
        return EDDS_OK;
    }
    status = edds_convert(source, EDDS_SOURCE_TGA, output, profile,
        never_cancelled, NULL, NULL, NULL, error);
    fclose(source);
    fclose(output);
    return status;
}

/** Blocks a mip of this size needs, which is the rule the padding exists to satisfy. */
static uint32_t block_count_of(uint32_t width, uint32_t height) {
    return ((width + 3u) / 4u) * ((height + 3u) / 4u);
}

static double mean_channel_error(
    const uint8_t *decoded,
    const uint8_t *expected,
    size_t pixels,
    const unsigned *channels,
    unsigned channel_count
) {
    double total = 0;
    for (size_t pixel = 0; pixel < pixels; ++pixel) {
        for (unsigned at = 0; at < channel_count; ++at) {
            const int difference = (int)decoded[pixel * 4u + channels[at]] -
                (int)expected[pixel * 4u + channels[at]];
            total += difference < 0 ? -difference : difference;
        }
    }
    return total / (double)(pixels * channel_count);
}

/** The gradient as straight RGBA, which is what a decode of a lossless result must equal. */
static void gradient_rgba(const uint8_t *bgra, uint8_t *rgba, size_t pixels) {
    for (size_t pixel = 0; pixel < pixels; ++pixel) {
        rgba[pixel * 4u] = bgra[pixel * 4u + 2u];
        rgba[pixel * 4u + 1u] = bgra[pixel * 4u + 1u];
        rgba[pixel * 4u + 2u] = bgra[pixel * 4u];
        rgba[pixel * 4u + 3u] = bgra[pixel * 4u + 3u];
    }
}

static int every_conversion_stores_its_proven_runtime_format(void) {
    static const struct {
        edds_conversion conversion;
        edds_pixel_format format;
        const char *four_cc;
        uint32_t dxgi;
        uint32_t block_bytes;
        uint32_t pixel_bytes;
        const char *channels;
    } expected[] = {
        { EDDS_CONVERSION_NONE, EDDS_PIXEL_BGRA8, "NONE", 0, 0, 4, "RGBA" },
        { EDDS_CONVERSION_DXT, EDDS_PIXEL_DXT5, "DXT5", 0, 16, 0, "RGBA" },
        { EDDS_CONVERSION_RED, EDDS_PIXEL_R8, "DX10", 61, 0, 1, "R" },
        { EDDS_CONVERSION_RED_HQ, EDDS_PIXEL_BC4, "DX10", 80, 8, 0, "R" },
        { EDDS_CONVERSION_RED_GREEN, EDDS_PIXEL_RG8, "DX10", 49, 0, 2, "RG" },
        { EDDS_CONVERSION_RED_GREEN_HQ, EDDS_PIXEL_BC5, "DX10", 83, 16, 0, "RG" },
        { EDDS_CONVERSION_COLOR_HQ, EDDS_PIXEL_BC7, "DX10", 98, 16, 0, "RGBA" }
    };
    uint8_t bgra[GRADIENT_PIXELS * 4];
    gradient_bgra(bgra, SOURCE_ALPHA_RAMP);

    for (size_t at = 0; at < sizeof expected / sizeof expected[0]; ++at) {
        edds_profile profile;
        edds_info info;
        edds_default_profile(&profile);
        profile.conversion = expected[at].conversion;
        profile.format_compress = EDDS_COMPRESS_COPY;
        CHECK(converted_source(bgra, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, &profile, 0,
            &info, NULL, NULL));
        CHECK(info.pixel_format == expected[at].format);
        CHECK(strcmp(info.four_cc, expected[at].four_cc) == 0);
        CHECK(info.dxgi_format == expected[at].dxgi);
        CHECK(strcmp(edds_pixel_format_channels(info.pixel_format), expected[at].channels) == 0);
        CHECK(info.preview_supported);
        /* Five mips: 9x5, 4x2, 2x1, 1x1 is four, and the chain ends at one by one. */
        CHECK(info.mip_count == 4u);
        for (uint32_t level = 0; level < info.mip_count; ++level) {
            const uint32_t width = info.mips[level].width;
            const uint32_t height = info.mips[level].height;
            const uint32_t bytes = expected[at].block_bytes != 0
                ? block_count_of(width, height) * expected[at].block_bytes
                : width * height * expected[at].pixel_bytes;
            CHECK(info.mips[level].decoded_bytes == bytes);
            CHECK(info.mips[level].container == EDDS_CONTAINER_COPY);
        }
        /* A block format declares its top mip as a linear size; an uncompressed one as a pitch. */
        CHECK(info.pitch_or_linear_size == (expected[at].block_bytes != 0
            ? block_count_of(GRADIENT_WIDTH, GRADIENT_HEIGHT) * expected[at].block_bytes
            : GRADIENT_WIDTH * expected[at].pixel_bytes));
    }
    return 1;
}

static int the_alpha_branch_is_read_off_each_source(void) {
    static const struct {
        source_alpha_shape shape;
        int with_alpha;
        edds_pixel_format uncompressed;
        edds_pixel_format compressed;
    } cases[] = {
        { SOURCE_NO_ALPHA_CHANNEL, 0, EDDS_PIXEL_BGRX8, EDDS_PIXEL_DXT1 },
        { SOURCE_FULLY_OPAQUE_ALPHA, 1, EDDS_PIXEL_BGRA8, EDDS_PIXEL_DXT1 },
        { SOURCE_ONE_SAMPLE_BELOW_OPAQUE, 1, EDDS_PIXEL_BGRA8, EDDS_PIXEL_DXT5 },
        { SOURCE_ALPHA_RAMP, 1, EDDS_PIXEL_BGRA8, EDDS_PIXEL_DXT5 }
    };
    for (size_t at = 0; at < sizeof cases / sizeof cases[0]; ++at) {
        uint8_t bgra[GRADIENT_PIXELS * 4];
        edds_profile profile;
        edds_info info;
        gradient_bgra(bgra, cases[at].shape);
        edds_default_profile(&profile);
        CHECK(converted_source(bgra, GRADIENT_WIDTH, GRADIENT_HEIGHT, cases[at].with_alpha,
            &profile, 0, &info, NULL, NULL));
        CHECK(info.pixel_format == cases[at].uncompressed);
        profile.conversion = EDDS_CONVERSION_DXT;
        CHECK(converted_source(bgra, GRADIENT_WIDTH, GRADIENT_HEIGHT, cases[at].with_alpha,
            &profile, 0, &info, NULL, NULL));
        CHECK(info.pixel_format == cases[at].compressed);
    }
    return 1;
}

/** Two sources in one batch each keep their own branch; nothing about it is shared. */
static int two_sources_under_one_profile_keep_their_own_alpha_branch(void) {
    uint8_t opaque[GRADIENT_PIXELS * 4];
    uint8_t translucent[GRADIENT_PIXELS * 4];
    edds_profile profile;
    edds_info first;
    edds_info second;
    gradient_bgra(opaque, SOURCE_FULLY_OPAQUE_ALPHA);
    gradient_bgra(translucent, SOURCE_ALPHA_RAMP);
    edds_default_profile(&profile);
    profile.conversion = EDDS_CONVERSION_DXT;
    CHECK(converted_source(opaque, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, &profile, 0,
        &first, NULL, NULL));
    CHECK(converted_source(translucent, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, &profile, 0,
        &second, NULL, NULL));
    CHECK(first.pixel_format == EDDS_PIXEL_DXT1 && second.pixel_format == EDDS_PIXEL_DXT5);
    return 1;
}

/** Red and RedGreen store the source channels themselves, so their decode is exact. */
static int the_uncompressed_channel_formats_are_lossless(void) {
    uint8_t bgra[GRADIENT_PIXELS * 4];
    uint8_t expected[GRADIENT_PIXELS * 4];
    edds_profile profile;
    edds_info info;
    uint8_t *rgba = NULL;
    size_t size = 0;
    gradient_bgra(bgra, SOURCE_ALPHA_RAMP);
    gradient_rgba(bgra, expected, GRADIENT_PIXELS);

    edds_default_profile(&profile);
    profile.conversion = EDDS_CONVERSION_RED;
    CHECK(converted_source(bgra, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, &profile, 0,
        &info, &rgba, &size));
    CHECK(size == GRADIENT_PIXELS * 4u);
    for (size_t pixel = 0; pixel < GRADIENT_PIXELS; ++pixel) {
        /* What the file holds, not what the source had: green and blue are simply not there. */
        CHECK(rgba[pixel * 4u] == expected[pixel * 4u]);
        CHECK(rgba[pixel * 4u + 1u] == 0 && rgba[pixel * 4u + 2u] == 0);
        CHECK(rgba[pixel * 4u + 3u] == 255u);
    }
    edds_free(rgba);
    rgba = NULL;

    profile.conversion = EDDS_CONVERSION_RED_GREEN;
    CHECK(converted_source(bgra, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, &profile, 0,
        &info, &rgba, &size));
    for (size_t pixel = 0; pixel < GRADIENT_PIXELS; ++pixel) {
        CHECK(rgba[pixel * 4u] == expected[pixel * 4u]);
        CHECK(rgba[pixel * 4u + 1u] == expected[pixel * 4u + 1u]);
        CHECK(rgba[pixel * 4u + 2u] == 0 && rgba[pixel * 4u + 3u] == 255u);
    }
    edds_free(rgba);
    return 1;
}

/** Every lossy conversion owes a bounded error on the channels it claims to carry. */
static int every_lossy_conversion_stays_inside_its_error_bound(void) {
    static const unsigned colour[] = { 0, 1, 2 };
    static const unsigned colour_alpha[] = { 0, 1, 2, 3 };
    static const unsigned red[] = { 0 };
    static const unsigned red_green[] = { 0, 1 };
    static const struct {
        edds_conversion conversion;
        const unsigned *channels;
        unsigned channel_count;
        double bound;
    } cases[] = {
        { EDDS_CONVERSION_DXT, colour_alpha, 4, 2.5 },
        { EDDS_CONVERSION_RED_HQ, red, 1, 1.0 },
        { EDDS_CONVERSION_RED_GREEN_HQ, red_green, 2, 1.0 },
        { EDDS_CONVERSION_COLOR_HQ, colour_alpha, 4, 2.5 }
    };
    uint8_t bgra[SMOOTH_PIXELS * 4];
    uint8_t expected[SMOOTH_PIXELS * 4];
    smooth_bgra(bgra);
    gradient_rgba(bgra, expected, SMOOTH_PIXELS);

    for (size_t at = 0; at < sizeof cases / sizeof cases[0]; ++at) {
        edds_profile profile;
        edds_info info;
        uint8_t *rgba = NULL;
        size_t size = 0;
        edds_default_profile(&profile);
        profile.conversion = cases[at].conversion;
        CHECK(converted_source(bgra, SMOOTH_SIDE, SMOOTH_SIDE, 1, &profile, 0,
            &info, &rgba, &size));
        CHECK(size == SMOOTH_PIXELS * 4u);
        CHECK(mean_channel_error(rgba, expected, SMOOTH_PIXELS,
            cases[at].channels, cases[at].channel_count) <= cases[at].bound);
        edds_free(rgba);
    }
    return 1;
}

/**
 * The point of the HQ conversion, stated as a test: given the same opaque colours, BC7 has to come
 * back closer to them than BC1 does. A BC7 encoder that only ever writes one line through a block
 * would pass every other test here and quietly fail this one.
 */
static int the_colour_hq_conversion_beats_dxt_on_the_same_colours(void) {
    static const unsigned colour[] = { 0, 1, 2 };
    uint8_t bgra[SMOOTH_PIXELS * 4];
    uint8_t expected[SMOOTH_PIXELS * 4];
    double dxt_error;
    double hq_error;
    smooth_bgra(bgra);
    for (size_t pixel = 0; pixel < SMOOTH_PIXELS; ++pixel) {
        bgra[pixel * 4u + 3u] = 255u;
    }
    gradient_rgba(bgra, expected, SMOOTH_PIXELS);

    {
        edds_profile profile;
        edds_info info;
        uint8_t *rgba = NULL;
        size_t size = 0;
        edds_default_profile(&profile);
        profile.conversion = EDDS_CONVERSION_DXT;
        CHECK(converted_source(bgra, SMOOTH_SIDE, SMOOTH_SIDE, 1, &profile, 0, &info, &rgba, &size));
        CHECK(info.pixel_format == EDDS_PIXEL_DXT1);
        dxt_error = mean_channel_error(rgba, expected, SMOOTH_PIXELS, colour, 3);
        edds_free(rgba);
        rgba = NULL;
        profile.conversion = EDDS_CONVERSION_COLOR_HQ;
        CHECK(converted_source(bgra, SMOOTH_SIDE, SMOOTH_SIDE, 1, &profile, 0, &info, &rgba, &size));
        CHECK(info.pixel_format == EDDS_PIXEL_BC7);
        hq_error = mean_channel_error(rgba, expected, SMOOTH_PIXELS, colour, 3);
        edds_free(rgba);
    }
    CHECK(hq_error < dxt_error);
    return 1;
}

static int conversion_quality_is_refused_where_nothing_proves_an_effect(void) {
    static const edds_conversion uncompressed[] = {
        EDDS_CONVERSION_NONE, EDDS_CONVERSION_RED, EDDS_CONVERSION_RED_GREEN
    };
    static const edds_conversion compressed[] = {
        EDDS_CONVERSION_DXT, EDDS_CONVERSION_RED_HQ,
        EDDS_CONVERSION_RED_GREEN_HQ, EDDS_CONVERSION_COLOR_HQ
    };
    edds_profile profile;
    edds_error error;

    for (size_t at = 0; at < sizeof uncompressed / sizeof uncompressed[0]; ++at) {
        edds_default_profile(&profile);
        profile.conversion = uncompressed[at];
        profile.conversion_quality = EDDS_QUALITY_SCALE / 2u;
        CHECK(refused_profile(&profile, &error) == EDDS_UNSUPPORTED_FORMAT);
        CHECK(strcmp(error.code, "unsupported-setting") == 0);
        /* The default is not a combination; it is what every conversion already has. */
        profile.conversion_quality = EDDS_QUALITY_SCALE;
        CHECK(edds_profile_check(&profile, &error) == EDDS_OK);
    }
    for (size_t at = 0; at < sizeof compressed / sizeof compressed[0]; ++at) {
        edds_default_profile(&profile);
        profile.conversion = compressed[at];
        profile.conversion_quality = 0;
        CHECK(edds_profile_check(&profile, &error) == EDDS_OK);
        profile.conversion_quality = EDDS_QUALITY_SCALE;
        CHECK(edds_profile_check(&profile, &error) == EDDS_OK);
        profile.conversion_quality = EDDS_QUALITY_SCALE + 1u;
        CHECK(refused_profile(&profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    }

    edds_default_profile(&profile);
    profile.conversion = EDDS_CONVERSION_HDR;
    CHECK(refused_profile(&profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strstr(error.message, "HDRCompression") != NULL);
    edds_default_profile(&profile);
    profile.conversion = (edds_conversion)99;
    CHECK(refused_profile(&profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    return 1;
}

/** Quality has to buy something, or it would be a control that changes nothing. */
static int conversion_quality_changes_a_compressed_result(void) {
    static const edds_conversion compressed[] = {
        EDDS_CONVERSION_DXT, EDDS_CONVERSION_RED_HQ,
        EDDS_CONVERSION_RED_GREEN_HQ, EDDS_CONVERSION_COLOR_HQ
    };
    static const unsigned channels[] = { 0, 1, 2, 3 };
    uint8_t bgra[GRADIENT_PIXELS * 4];
    uint8_t expected[GRADIENT_PIXELS * 4];
    gradient_bgra(bgra, SOURCE_ALPHA_RAMP);
    gradient_rgba(bgra, expected, GRADIENT_PIXELS);

    for (size_t at = 0; at < sizeof compressed / sizeof compressed[0]; ++at) {
        edds_profile profile;
        edds_info cheap;
        edds_info dear;
        uint8_t *cheap_rgba = NULL;
        uint8_t *dear_rgba = NULL;
        size_t cheap_size = 0;
        size_t dear_size = 0;
        double cheap_error;
        double dear_error;
        edds_default_profile(&profile);
        profile.conversion = compressed[at];
        profile.conversion_quality = 0;
        CHECK(converted_source(bgra, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, &profile, 0,
            &cheap, &cheap_rgba, &cheap_size));
        profile.conversion_quality = EDDS_QUALITY_SCALE;
        CHECK(converted_source(bgra, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, &profile, 0,
            &dear, &dear_rgba, &dear_size));
        cheap_error = mean_channel_error(cheap_rgba, expected, GRADIENT_PIXELS, channels, 4);
        dear_error = mean_channel_error(dear_rgba, expected, GRADIENT_PIXELS, channels, 4);
        CHECK(cheap_size == dear_size);
        CHECK(dear_error < cheap_error);
        edds_free(cheap_rgba);
        edds_free(dear_rgba);
    }
    return 1;
}

/**
 * `FormatCompress` is a container, not a conversion. The same profile through COPY and through
 * LZ4 has to decode to the same bytes, or one of the two is changing pixels behind the setting.
 */
static int container_compression_never_changes_a_decoded_pixel(void) {
    static const edds_conversion conversions[] = {
        EDDS_CONVERSION_NONE, EDDS_CONVERSION_DXT, EDDS_CONVERSION_RED,
        EDDS_CONVERSION_RED_HQ, EDDS_CONVERSION_RED_GREEN,
        EDDS_CONVERSION_RED_GREEN_HQ, EDDS_CONVERSION_COLOR_HQ
    };
    uint8_t bgra[SMOOTH_PIXELS * 4];
    quadrant_bgra(bgra);

    for (size_t at = 0; at < sizeof conversions / sizeof conversions[0]; ++at) {
        edds_profile profile;
        edds_info copied;
        edds_info compressed;
        uint8_t *copied_rgba = NULL;
        uint8_t *compressed_rgba = NULL;
        size_t copied_size = 0;
        size_t compressed_size = 0;
        edds_default_profile(&profile);
        profile.conversion = conversions[at];
        profile.format_compress = EDDS_COMPRESS_COPY;
        CHECK(converted_source(bgra, SMOOTH_SIDE, SMOOTH_SIDE, 1, &profile, 0,
            &copied, &copied_rgba, &copied_size));
        profile.format_compress = EDDS_COMPRESS_BEST;
        profile.compress_threshold = 100;
        CHECK(converted_source(bgra, SMOOTH_SIDE, SMOOTH_SIDE, 1, &profile, 0,
            &compressed, &compressed_rgba, &compressed_size));
        CHECK(copied.pixel_format == compressed.pixel_format);
        CHECK(copied.mips[0].container == EDDS_CONTAINER_COPY);
        CHECK(compressed.mips[0].container == EDDS_CONTAINER_LZ4);
        CHECK(copied_size == compressed_size);
        CHECK(memcmp(copied_rgba, compressed_rgba, copied_size) == 0);
        edds_free(copied_rgba);
        edds_free(compressed_rgba);
    }
    return 1;
}

/**
 * A mip narrower than a block still costs a whole one, and the smallest mip of every chain is a
 * single pixel inside a single block. Both are decoded back to exactly their own dimensions.
 */
static int block_padding_reaches_the_smallest_mip(void) {
    static const edds_conversion conversions[] = {
        EDDS_CONVERSION_DXT, EDDS_CONVERSION_RED_HQ,
        EDDS_CONVERSION_RED_GREEN_HQ, EDDS_CONVERSION_COLOR_HQ
    };
    uint8_t bgra[GRADIENT_PIXELS * 4];
    gradient_bgra(bgra, SOURCE_ALPHA_RAMP);

    for (size_t at = 0; at < sizeof conversions / sizeof conversions[0]; ++at) {
        edds_profile profile;
        edds_info info;
        edds_default_profile(&profile);
        profile.conversion = conversions[at];
        CHECK(converted_source(bgra, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, &profile, 0,
            &info, NULL, NULL));
        CHECK(info.mip_count == 4u);
        CHECK(info.mips[0].width == 9 && info.mips[0].height == 5);
        CHECK(info.mips[1].width == 4 && info.mips[1].height == 2);
        CHECK(info.mips[2].width == 2 && info.mips[2].height == 1);
        CHECK(info.mips[3].width == 1 && info.mips[3].height == 1);
        /* Three block columns and two block rows carry a nine-by-five image. */
        CHECK(info.mips[0].decoded_bytes == 6u * (conversions[at] == EDDS_CONVERSION_RED_HQ ? 8u : 16u));
        for (uint32_t level = 1; level < info.mip_count; ++level) {
            CHECK(info.mips[level].decoded_bytes ==
                (conversions[at] == EDDS_CONVERSION_RED_HQ ? 8u : 16u));
        }
        for (uint32_t level = 0; level < info.mip_count; ++level) {
            uint8_t *rgba = NULL;
            size_t size = 0;
            edds_info reread;
            CHECK(converted_source(bgra, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, &profile, level,
                &reread, &rgba, &size));
            CHECK(size == (size_t)reread.mips[level].width * reread.mips[level].height * 4u);
            edds_free(rgba);
        }
    }
    return 1;
}

/** A block payload that is not whole blocks is refused at inspection, before any decode. */
static int truncated_gpu_blocks_are_refused(void) {
    uint8_t bgra[GRADIENT_PIXELS * 4];
    uint8_t tga[18u + GRADIENT_PIXELS * 4u];
    uint8_t *converted = NULL;
    size_t converted_size = 0;
    FILE *source;
    FILE *output = temporary();
    edds_profile profile;
    edds_error error;
    edds_info info;
    long size;

    gradient_bgra(bgra, SOURCE_ALPHA_RAMP);
    source = stream_of(tga,
        fixture_tga_build(tga, sizeof tga, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, bgra));
    edds_default_profile(&profile);
    profile.conversion = EDDS_CONVERSION_COLOR_HQ;
    profile.format_compress = EDDS_COMPRESS_COPY;
    profile.generate_mips = 0;
    CHECK(source != NULL && output != NULL);
    CHECK(edds_convert(source, EDDS_SOURCE_TGA, output, &profile,
        never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);
    fclose(source);
    CHECK(fseek(output, 0, SEEK_END) == 0 && (size = ftell(output)) > 0);
    converted_size = (size_t)size;
    converted = malloc(converted_size);
    CHECK(converted != NULL && fseek(output, 0, SEEK_SET) == 0);
    CHECK(fread(converted, 1, converted_size, output) == converted_size);
    fclose(output);

    /* One block short of what three by two blocks require, declared in the mip table itself. */
    {
        FILE *damaged;
        const size_t table_at = 148u;
        converted[table_at + 4u] = (uint8_t)(6u * 16u - 16u);
        damaged = stream_of(converted, converted_size - 16u);
        CHECK(damaged != NULL);
        CHECK(edds_inspect(damaged, &info, never_cancelled, NULL, &error) == EDDS_INVALID_INPUT);
        CHECK(strcmp(error.code, "unexpected-mip-size") == 0);
        fclose(damaged);
    }
    free(converted);
    return 1;
}

/** Every conversion and every quality the CLI accepts survives a trip through the metadata text. */
static int every_conversion_round_trips_through_metadata(void) {
    static const uint32_t qualities[] = { 0, 26, 30, 500, 403, EDDS_QUALITY_SCALE };
    size_t count = 0;
    const edds_conversion_capability *capabilities = edds_conversions(&count);
    CHECK(count == 8u);

    for (size_t at = 0; at < count; ++at) {
        for (size_t quality = 0; quality < sizeof qualities / sizeof qualities[0]; ++quality) {
            edds_metadata metadata;
            edds_metadata parsed;
            edds_error error;
            FILE *written;
            memset(&metadata, 0, sizeof metadata);
            memcpy(metadata.guid, "0123456789ABCDEF", 17);
            (void)snprintf(metadata.name, sizeof metadata.name, "Probe/pixel.edds");
            (void)snprintf(metadata.source_file, sizeof metadata.source_file, "pixel.tga");
            metadata.source_format = EDDS_SOURCE_TGA;
            edds_default_profile(&metadata.profile);
            metadata.profile.conversion = capabilities[at].conversion;
            metadata.profile.conversion_quality = qualities[quality];
            written = temporary();
            CHECK(written != NULL);
            if (!capabilities[at].supported ||
                (!capabilities[at].uses_quality && qualities[quality] != EDDS_QUALITY_SCALE)) {
                /* Canonical metadata is never allowed to record a recipe that cannot be run. */
                CHECK(edds_metadata_write(written, &metadata, &error) != EDDS_OK);
                fclose(written);
                continue;
            }
            CHECK(edds_metadata_write(written, &metadata, &error) == EDDS_OK);
            CHECK(fseek(written, 0, SEEK_SET) == 0);
            CHECK(edds_metadata_parse(written, &parsed, &error) == EDDS_OK);
            CHECK(parsed.profile.conversion == metadata.profile.conversion);
            CHECK(parsed.profile.conversion_quality == metadata.profile.conversion_quality);
            fclose(written);
        }
    }
    return 1;
}

/** A quality the text cannot state exactly is refused rather than rounded into something else. */
static int metadata_quality_text_is_exact_or_refused(void) {
    static const struct {
        const char *text;
        int accepted;
        uint32_t thousandths;
    } cases[] = {
        { "1", 1, 1000 }, { "0", 1, 0 }, { "0.5", 1, 500 }, { "0.403", 1, 403 },
        { "0.026", 1, 26 }, { "1.000", 1, 1000 }, { "0.0260", 0, 0 }, { "1.5", 0, 0 },
        { "2", 0, 0 }, { "0.", 0, 0 }, { ".5", 0, 0 }, { "-1", 0, 0 }, { "0.5x", 0, 0 }
    };
    for (size_t at = 0; at < sizeof cases / sizeof cases[0]; ++at) {
        char source[256];
        FILE *input;
        edds_metadata metadata;
        edds_error error;
        (void)snprintf(source, sizeof source,
            "MetaFileClass { Name \"{0123456789ABCDEF}a.edds\" Configurations { "
            "TGAResourceClass PC { SourceFile \"a.tga\" Conversion DXTCompression "
            "ConversionQuality %s } } }", cases[at].text);
        input = stream_of((const uint8_t *)source, strlen(source));
        CHECK(input != NULL);
        if (cases[at].accepted) {
            CHECK(edds_metadata_parse(input, &metadata, &error) == EDDS_OK);
            CHECK(metadata.profile.conversion_quality == cases[at].thousandths);
            CHECK(metadata.profile.conversion == EDDS_CONVERSION_DXT);
        } else {
            CHECK(edds_metadata_parse(input, &metadata, &error) != EDDS_OK);
        }
        fclose(input);
    }
    return 1;
}

/** The conversion contract is one table, and everything that names a conversion reads it. */
static int the_conversion_contract_is_one_table(void) {
    size_t count = 0;
    const edds_conversion_capability *capabilities = edds_conversions(&count);
    CHECK(capabilities != NULL && count == 8u);
    for (size_t at = 0; at < count; ++at) {
        CHECK(edds_conversion_capability_of(capabilities[at].conversion) == &capabilities[at]);
        CHECK(edds_conversion_of_workbench_name(capabilities[at].workbench_name) == &capabilities[at]);
        CHECK(edds_conversion_of_wire_name(capabilities[at].wire_name) == &capabilities[at]);
    }
    CHECK(edds_conversion_of_workbench_name("DXT") == NULL);
    CHECK(edds_conversion_of_workbench_name(NULL) == NULL);
    CHECK(edds_conversion_of_wire_name("dxt") == NULL);
    CHECK(edds_conversion_capability_of((edds_conversion)99) == NULL);
    return 1;
}

/** A stored DXT1 block is pixels now, so an existing DayZ texture previews instead of refusing. */
static int a_stored_dxt1_block_decodes_to_its_pixels(void) {
    test_bytes fixture = fixture_dxt1();
    FILE *file = stream_of(fixture.data, fixture.size);
    edds_info info;
    edds_error error;
    uint8_t *rgba = NULL;
    size_t size = 0;

    CHECK(fixture.data != NULL && file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.pixel_format == EDDS_PIXEL_DXT1 && info.mips[0].stored_bytes == 8);
    CHECK(info.preview_supported);
    CHECK(edds_preview(file, &info, 0, never_cancelled, NULL, &rgba, &size, &error) == EDDS_OK);
    CHECK(size == 4u * 4u * 4u);
    /* Both endpoints of the block are black, so every pixel of it is opaque black. */
    CHECK(every_pixel_is(rgba, size, 0, 0, 0));

    edds_free(rgba);
    fclose(file);
    fixture_free(fixture);
    return 1;
}

/**
 * A BC3 colour block is always four colours. Reading it as BC1 would turn its fourth entry into a
 * transparent black, so a texture another tool wrote with the endpoints the other way round would
 * preview with holes in it where the engine shows colour.
 */
static int a_dxt5_colour_block_is_never_read_as_punch_through(void) {
    test_bytes fixture = fixture_dxt5_low_endpoints();
    FILE *file = stream_of(fixture.data, fixture.size);
    edds_info info;
    edds_error error;
    uint8_t *rgba = NULL;
    size_t size = 0;

    CHECK(fixture.data != NULL && file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.pixel_format == EDDS_PIXEL_DXT5);
    CHECK(edds_preview(file, &info, 0, never_cancelled, NULL, &rgba, &size, &error) == EDDS_OK);
    CHECK(size == 4u * 4u * 4u);
    /* Black and red endpoints, every index the last one: a third of the way from red to black. */
    CHECK(every_pixel_is(rgba, size, 170, 0, 0));

    edds_free(rgba);
    fclose(file);
    fixture_free(fixture);
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
        mip_profile_refuses_conflicts_and_unproven_values() &&
        dds_supplied_levels_are_removed_from_the_large_end() &&
        dds_top_level_generates_but_cannot_claim_a_supplied_chain() &&
        dds_refusals_are_precise_and_atomic() &&
        dds_source_matrix_covers_legacy_blocks_and_dx10_supplied_levels() &&
        dds_alpha_branch_scans_every_supplied_level() &&
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
        damaged_tiff_input_fails_without_partial_output() &&
        the_conversion_contract_is_one_table() &&
        box_and_kaiser_mips_cover_npot_edges_exactly() &&
        normalize_flag_and_mipmap_function_have_distinct_stages() &&
        every_conversion_stores_its_proven_runtime_format() &&
        the_alpha_branch_is_read_off_each_source() &&
        two_sources_under_one_profile_keep_their_own_alpha_branch() &&
        the_uncompressed_channel_formats_are_lossless() &&
        every_lossy_conversion_stays_inside_its_error_bound() &&
        the_colour_hq_conversion_beats_dxt_on_the_same_colours() &&
        conversion_quality_is_refused_where_nothing_proves_an_effect() &&
        conversion_quality_changes_a_compressed_result() &&
        container_compression_never_changes_a_decoded_pixel() &&
        block_padding_reaches_the_smallest_mip() &&
        truncated_gpu_blocks_are_refused() &&
        every_conversion_round_trips_through_metadata() &&
        metadata_quality_text_is_exact_or_refused() &&
        a_stored_dxt1_block_decodes_to_its_pixels() &&
        a_dxt5_colour_block_is_never_read_as_punch_through();

    return passed ? 0 : 1;
}
