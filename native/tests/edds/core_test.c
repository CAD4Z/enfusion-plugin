#include <edds/edds.h>
#include <edds/batch.h>
#include <edds/pool.h>

#include "fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Ends the running test with 0 when a check fails, after printing its file, line and text. */
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            return 0; \
        } \
    } while (0)

/**
 * A new temporary file, open for reading and writing, or NULL when none can be made: tmpfile_s on
 * Windows, tmpfile elsewhere.
 */
static FILE *temporary(void) {
#ifdef _WIN32
    FILE *file = NULL;

    return tmpfile_s(&file) == 0 ? file : NULL;
#else
    return tmpfile();
#endif
}

/**
 * A temporary file holding a copy of the bytes, positioned at its start, or NULL when it cannot be
 * made or written. The caller closes it.
 */
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

/** A cancellation check that is never true. */
static int never_cancelled(void *context) {
    (void)context;

    return 0;
}

/** A cancellation check that is always true. */
static int always_cancelled(void *context) {
    (void)context;

    return 1;
}

/**
 * The COPY fixture, a 3x2 BGRA image with one mip, inspects to its exact layout, and its top level
 * previews as RGBA.
 */
static int copy_inspection_and_preview(void) {
    /* The fixture, as a file. */
    test_bytes fixture = fixture_copy_bgra();
    FILE      *file    = stream_of(fixture.data, fixture.size);

    /* What inspection reads, and the error a failed call fills in. */
    edds_info  info;
    edds_error error;

    /* The previewed level. */
    uint8_t *rgba = NULL;
    size_t   size = 0;

    /* Its first two pixels: the stored BGRA bytes with red and blue swapped. */
    static const uint8_t expected_first_two[] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    CHECK(fixture.data != NULL && file != NULL);

    /* The header: 3x2 BGRA over two levels, in 128 bytes. */
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.width == 3 && info.height == 2 && info.mip_count == 2);
    CHECK(info.pixel_format == EDDS_PIXEL_BGRA8 && info.header_bytes == 128);

    /* Level 0, 3x2, is 24 bytes stored as COPY at 148: after level 1, which is at 144. */
    CHECK(info.mips[0].level == 0 && info.mips[0].width == 3 && info.mips[0].height == 2);
    CHECK(info.mips[0].container == EDDS_CONTAINER_COPY);
    CHECK(info.mips[0].stored_bytes == 24 && info.mips[0].data_offset == 148);
    CHECK(info.mips[1].level == 1 && info.mips[1].data_offset == 144);

    /* Level 0 previewed: 24 bytes of RGBA. */
    CHECK(edds_preview(file, &info, 0, never_cancelled, NULL, &rgba, &size, &error) == EDDS_OK);
    CHECK(size == 24 && memcmp(rgba, expected_first_two, sizeof expected_first_two) == 0);

    edds_free(rgba);
    fclose(file);
    fixture_free(fixture);

    return 1;
}

/**
 * The LZ4 fixture, a 2x1 BGRX image in one block, inspects as BGRX and previews with both alphas
 * at 255, though the fourth bytes it stores are 0 and 17.
 */
static int lz4_preview_and_bgrx_alpha(void) {
    /* The fixture, as a file. */
    test_bytes fixture = fixture_lz4_bgrx();
    FILE      *file    = stream_of(fixture.data, fixture.size);

    /* What inspection reads, and the error a failed call fills in. */
    edds_info  info;
    edds_error error;

    /* The previewed level. */
    uint8_t *rgba = NULL;
    size_t   size = 0;

    /* The RGBA it must hold. */
    static const uint8_t expected[] = { 1, 2, 3, 255, 4, 5, 6, 255 };

    CHECK(fixture.data != NULL && file != NULL);

    /* BGRX, in one LZ4 block. */
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

/**
 * A 256x65 BGRX level in two LZ4 blocks, the second of which draws on the first as its
 * dictionary, decodes whole: the same opaque pixel at both ends.
 */
static int lz4_stream_keeps_the_previous_dictionary(void) {
    /* The fixture, as a file. */
    test_bytes fixture = fixture_lz4_streaming_bgrx();
    FILE      *file    = stream_of(fixture.data, fixture.size);

    /* What inspection reads, and the error a failed call fills in. */
    edds_info  info;
    edds_error error;

    /* The previewed level. */
    uint8_t *rgba = NULL;
    size_t   size = 0;

    CHECK(fixture.data != NULL && file != NULL);

    /* Two blocks, which decode to the whole level. */
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.mips[0].block_count == 2 && info.mips[0].decoded_bytes == 256u * 65u * 4u);

    /* The first and the last pixel: red 1, green 2, blue 3, alpha 255. */
    CHECK(edds_preview(file, &info, 0, never_cancelled, NULL, &rgba, &size, &error) == EDDS_OK);
    CHECK(size == 256u * 65u * 4u);
    CHECK(rgba[0] == 1 && rgba[1] == 2 && rgba[2] == 3 && rgba[3] == 255);
    CHECK(rgba[size - 4u] == 1 && rgba[size - 1u] == 255);

    edds_free(rgba);
    fclose(file);
    fixture_free(fixture);

    return 1;
}

/**
 * A 2x1 BGRX image behind a DX10 header (DXGI format 93, resource dimension 3, array size 1):
 * the header takes 148 bytes, and the level previews as RGBA with both alphas at 255.
 */
static int dx10_bgrx_is_a_single_surface_preview(void) {
    /* The fixture, as a file. */
    test_bytes fixture = fixture_dx10_bgrx();
    FILE      *file    = stream_of(fixture.data, fixture.size);

    /* What inspection reads, and the error a failed call fills in. */
    edds_info  info;
    edds_error error;

    /* The previewed level. */
    uint8_t *rgba = NULL;
    size_t   size = 0;

    /* The RGBA it must hold. */
    static const uint8_t expected[] = { 10, 20, 30, 255, 40, 50, 60, 255 };

    CHECK(fixture.data != NULL && file != NULL);

    /* The DX10 header, read as BGRX that can be previewed. */
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

/**
 * A 4x4 image whose FourCC names no known format still inspects, with its 8-byte level, but its
 * preview is refused as unsupported and returns no pixels.
 */
static int unsupported_pixels_keep_the_inspection(void) {
    /* The fixture, as a file. */
    test_bytes fixture = fixture_odd_fourcc();
    FILE      *file    = stream_of(fixture.data, fixture.size);

    /* What inspection reads, and the error a failed call fills in. */
    edds_info  info;
    edds_error error;

    /* The preview, which must stay empty. */
    uint8_t *rgba = NULL;
    size_t   size = 0;

    CHECK(fixture.data != NULL && file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.pixel_format == EDDS_PIXEL_UNKNOWN && info.mips[0].stored_bytes == 8);

    CHECK(edds_preview(file, &info, 0, never_cancelled, NULL, &rgba, &size, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(rgba == NULL && strstr(error.code, "unsupported") != NULL);

    fclose(file);
    fixture_free(fixture);

    return 1;
}

/**
 * The pixel-format flags at byte 80 traded between two fixtures: the BGRA one gets 0x40, the flags
 * of the BGRX one, while it keeps its alpha mask, and the BGRX one gets 0x41 with no alpha mask.
 * Both still inspect, as an unknown format that cannot be previewed.
 */
static int contradictory_legacy_flags_never_produce_pixels(void) {
    test_bytes bgra = fixture_copy_bgra();
    test_bytes bgrx = fixture_lz4_bgrx();
    edds_info  info;
    edds_error error;
    FILE      *file;

    CHECK(bgra.data != NULL && bgrx.data != NULL);

    /* An alpha mask under flags that declare none. */
    bgra.data[80] = 0x40;
    file          = stream_of(bgra.data, bgra.size);
    CHECK(file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.pixel_format == EDDS_PIXEL_UNKNOWN && !info.preview_supported);
    fclose(file);

    /* Flags that declare alpha over no alpha mask. */
    bgrx.data[80] = 0x41;
    file          = stream_of(bgrx.data, bgrx.size);
    CHECK(file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.pixel_format == EDDS_PIXEL_UNKNOWN && !info.preview_supported);
    fclose(file);

    fixture_free(bgra);
    fixture_free(bgrx);

    return 1;
}

/**
 * Every prefix of the COPY fixture shorter than the whole file fails inspection as invalid input,
 * and so does the whole file once its width is 65537, past the dimension limit.
 */
static int truncation_and_limits_fail_without_output(void) {
    test_bytes fixture = fixture_copy_bgra();
    edds_info  info;
    edds_error error;

    CHECK(fixture.data != NULL);

    for (size_t length = 0; length < fixture.size; ++length) {
        FILE *file = stream_of(fixture.data, length);

        CHECK(file != NULL);
        CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_INVALID_INPUT);
        fclose(file);
    }

    /* The width, bytes 16 to 19, set to 0x00010001. */
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

/**
 * The LZ4 fixture: inspected with a cancellation check that is already true, it returns
 * EDDS_CANCELLED; with its only block no longer marked final, it fails as invalid input.
 */
static int malformed_lz4_and_cancellation_are_bounded(void) {
    test_bytes fixture = fixture_lz4_bgrx();
    edds_info  info;
    edds_error error;
    FILE      *file;

    CHECK(fixture.data != NULL);

    /* Cancelled from the start. */
    file = stream_of(fixture.data, fixture.size);
    CHECK(file != NULL);
    CHECK(edds_inspect(file, &info, always_cancelled, NULL, &error) == EDDS_CANCELLED);
    fclose(file);

    /* The block header's top byte cleared, and with it the bit that marks the final block. */
    fixture.data[143] = 0;

    file = stream_of(fixture.data, fixture.size);
    CHECK(file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_INVALID_INPUT);
    fclose(file);

    fixture_free(fixture);

    return 1;
}

/**
 * Three inputs refused as invalid: the COPY fixture claiming 0xffffffff stored bytes for a level,
 * the LZ4 fixture claiming 0xffffffff decoded bytes, and a file larger than EDDS_MAX_FILE_BYTES.
 */
static int malformed_sizes_and_oversized_files_are_refused(void) {
    test_bytes copy = fixture_copy_bgra();
    test_bytes lz4  = fixture_lz4_bgrx();
    edds_info  info;
    edds_error error;
    FILE      *file;

    CHECK(copy.data != NULL && lz4.data != NULL);

    /* The stored size in the first entry of the mip table, bytes 132 to 135. */
    memset(copy.data + 132, 0xff, 4);
    file = stream_of(copy.data, copy.size);
    CHECK(file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_INVALID_INPUT);
    fclose(file);

    /* The decoded size that opens the LZ4 payload, bytes 136 to 139. */
    memset(lz4.data + 136, 0xff, 4);
    file = stream_of(lz4.data, lz4.size);
    CHECK(file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_INVALID_INPUT);
    fclose(file);

    /* One byte written past the limit makes the file EDDS_MAX_FILE_BYTES + 2 bytes long. */
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

/**
 * Header values of 0xffffffff, refused with their own error codes: a width and height as
 * "dimension-limit", and a mip count as "mip-count-limit".
 */
static int integer_boundaries_are_refused_before_arithmetic(void) {
    test_bytes dimensions = fixture_integer_overflow();
    test_bytes mips       = fixture_copy_bgra();
    edds_info  info;
    edds_error error;
    FILE      *file;

    CHECK(dimensions.data != NULL && mips.data != NULL);

    /* The height and width, bytes 12 to 19, every byte 0xff. */
    file = stream_of(dimensions.data, dimensions.size);
    CHECK(file != NULL);
    CHECK(edds_inspect(file, &info, never_cancelled, NULL, &error) == EDDS_INVALID_INPUT);
    CHECK(strcmp(error.code, "dimension-limit") == 0);
    fclose(file);

    /* The mip count, bytes 28 to 31. */
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

/**
 * A 3x2 24-bit TGA converted with the default profile, its levels stored as COPY: the output is
 * BGRX with a generated 1x1 level, and the two levels preview as the samples, opaque, and as their
 * average.
 */
static int uncompressed_tga_converts_through_the_public_edds_seam(void) {
    /* The TGA: an 18-byte header (3x2, 24 bits, top row first), then the BGR samples. */
    static const uint8_t tga[] = {
        0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        3, 0, 2, 0, 24, 0x20,
        30, 20, 10, 60, 50, 40, 90, 80, 70,
        120, 110, 100, 150, 140, 130, 180, 170, 160
    };

    /* The RGBA of the two levels. */
    static const uint8_t expected_mip_zero[] = {
        10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255,
        100, 110, 120, 255, 130, 140, 150, 255, 160, 170, 180, 255
    };
    static const uint8_t expected_mip_one[] = { 85, 95, 105, 255 };

    /* The source, and the converted file. */
    FILE *source = stream_of(tga, sizeof tga);
    FILE *output = temporary();

    /* The conversion settings, the inspected output, and the error a failed call fills in. */
    edds_profile profile;
    edds_info    info;
    edds_error   error;

    /* A previewed level. */
    uint8_t *rgba      = NULL;
    size_t   rgba_size = 0;

    CHECK(source != NULL && output != NULL);

    /* Converted with the default profile, the levels stored as COPY. */
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    CHECK(edds_convert(source, EDDS_SOURCE_TGA, output, &profile, never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);

    /* 3x2 BGRX in two levels: the top one stored as COPY, the second 1x1. */
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.width == 3 && info.height == 2 && info.mip_count == 2);
    CHECK(info.pixel_format == EDDS_PIXEL_BGRX8);
    CHECK(info.mips[0].container == EDDS_CONTAINER_COPY);
    CHECK(info.mips[1].width == 1 && info.mips[1].height == 1);

    /* Level 0: the samples, opaque. */
    CHECK(edds_preview(output, &info, 0, never_cancelled, NULL, &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == sizeof expected_mip_zero && memcmp(rgba, expected_mip_zero, sizeof expected_mip_zero) == 0);
    edds_free(rgba);
    rgba = NULL;

    /* Level 1: their average. */
    CHECK(edds_preview(output, &info, 1, never_cancelled, NULL, &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == sizeof expected_mip_one && memcmp(rgba, expected_mip_one, sizeof expected_mip_one) == 0);

    edds_free(rgba);
    fclose(source);
    fclose(output);

    return 1;
}

/**
 * The default profile, field by field: FASTEST compression at a threshold of 80, mips generated
 * rather than supplied, none removed, made by the Filter function with the Box filter, no swizzle,
 * no normalize, and a tiled texture.
 */
static int supported_workbench_defaults_are_explicit(void) {
    edds_profile profile;

    edds_default_profile(&profile);
    CHECK(profile.format_compress == EDDS_COMPRESS_FASTEST);
    CHECK(profile.compress_threshold == 80u);
    CHECK(profile.remove_mips == 0u);
    CHECK(profile.swizzling == EDDS_SWIZZLE_NONE);
    CHECK(profile.contains_mips == 0);
    CHECK(profile.generate_mips == 1);
    CHECK(profile.normalize == 0);
    CHECK(profile.mipmap_function == EDDS_MIPMAP_FILTER);
    CHECK(profile.mipmap_filter == EDDS_FILTER_BOX);
    CHECK(profile.tiled_texture == 1);

    return 1;
}

/**
 * Single changes to the default profile, each checked by edds_profile_check: the conflicting and
 * unsupported ones are refused with their own error, the supported ones pass.
 */
static int mip_profile_refuses_conflicts_and_unproven_values(void) {
    edds_profile profile;
    edds_error   error;

    /* Mips both supplied by the source and generated. */
    edds_default_profile(&profile);
    profile.contains_mips = 1;
    CHECK(edds_profile_check(&profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-combination") == 0);

    /* Fifteen levels removed. */
    edds_default_profile(&profile);
    profile.remove_mips = 15;
    CHECK(edds_profile_check(&profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-setting") == 0);

    /* The ColorNoise function with the Kaiser filter passes. */
    edds_default_profile(&profile);
    profile.mipmap_function = EDDS_MIPMAP_COLOR_NOISE;
    profile.mipmap_filter   = EDDS_FILTER_KAISER;
    CHECK(edds_profile_check(&profile, &error) == EDDS_OK);

    /* The Triangle filter is refused by name. */
    edds_default_profile(&profile);
    profile.mipmap_filter = EDDS_FILTER_TRIANGLE;
    CHECK(edds_profile_check(&profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strstr(error.message, "Triangle") != NULL);

    /* The Normalize function with no mips generated. */
    edds_default_profile(&profile);
    profile.generate_mips   = 0;
    profile.mipmap_function = EDDS_MIPMAP_NORMALIZE;
    CHECK(edds_profile_check(&profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-combination") == 0);

    /* The Normalize function with the Kaiser filter. */
    edds_default_profile(&profile);
    profile.mipmap_function = EDDS_MIPMAP_NORMALIZE;
    profile.mipmap_filter   = EDDS_FILTER_KAISER;
    CHECK(edds_profile_check(&profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-combination") == 0);

    /* An untiled texture passes; a tiled value of 2 does not. */
    edds_default_profile(&profile);
    profile.tiled_texture = 0;
    CHECK(edds_profile_check(&profile, &error) == EDDS_OK);
    profile.tiled_texture = 2;
    CHECK(edds_profile_check(&profile, &error) == EDDS_UNSUPPORTED_FORMAT);

    return 1;
}

/**
 * The 4x2 BGRX DDS with three levels, converted with the mips it supplies and one level removed:
 * the output starts at the 2x1 middle level and keeps the 1x1 last one.
 */
static int dds_supplied_levels_are_removed_from_the_large_end(void) {
    /* The RGBA of the two levels kept: the DDS's middle and last levels, opaque. */
    static const uint8_t expected_middle[] = {
        11, 22, 33, 255, 44, 55, 66, 255
    };
    static const uint8_t expected_last[] = { 77, 88, 99, 255 };

    /* The DDS source, and the converted file. */
    test_bytes dds    = fixture_dds_bgrx_mips();
    FILE      *source = stream_of(dds.data, dds.size);
    FILE      *output = temporary();

    /* The conversion settings, the inspected output, and the error a failed call fills in. */
    edds_profile profile;
    edds_info    info;
    edds_error   error;

    /* A previewed level. */
    uint8_t *rgba      = NULL;
    size_t   rgba_size = 0;

    CHECK(dds.data != NULL && source != NULL && output != NULL);

    /* Converted with the levels the DDS supplies, the largest removed, stored as COPY. */
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    profile.contains_mips   = 1;
    profile.generate_mips   = 0;
    profile.remove_mips     = 1;
    CHECK(edds_convert(source, EDDS_SOURCE_DDS, output, &profile, never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);

    /* The output is 2x1, in two levels. */
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.width == 2 && info.height == 1 && info.mip_count == 2);

    /* Level 0 is the DDS's middle level. */
    CHECK(edds_preview(output, &info, 0, never_cancelled, NULL, &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == sizeof expected_middle && memcmp(rgba, expected_middle, sizeof expected_middle) == 0);
    edds_free(rgba);
    rgba = NULL;

    /* Level 1 is its last. */
    CHECK(edds_preview(output, &info, 1, never_cancelled, NULL, &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == sizeof expected_last && memcmp(rgba, expected_last, sizeof expected_last) == 0);

    edds_free(rgba);
    fclose(source);
    fclose(output);
    fixture_free(dds);

    return 1;
}

/**
 * The 3x2 RGBA PNG converted with its levels stored as COPY: the output is BGRA in two levels,
 * the top one the PNG's samples and the 1x1 one their average, alpha included.
 */
static int rgba_png_converts_with_declared_alpha(void) {
    /* The RGBA of the two levels. */
    static const uint8_t expected_mip_zero[] = {
        10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120,
        110, 120, 130, 140, 150, 160, 170, 180, 190, 200, 210, 220
    };
    static const uint8_t expected_mip_one[] = { 100, 110, 120, 130 };

    /* The PNG source, and the converted file. */
    test_bytes png    = fixture_png_rgba();
    FILE      *source = stream_of(png.data, png.size);
    FILE      *output = temporary();

    /* The conversion settings, the inspected output, and the error a failed call fills in. */
    edds_profile profile;
    edds_info    info;
    edds_error   error;

    /* A previewed level. */
    uint8_t *rgba      = NULL;
    size_t   rgba_size = 0;

    CHECK(png.data != NULL && source != NULL && output != NULL);

    /* Converted with the default profile, the levels stored as COPY. */
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    CHECK(edds_convert(source, EDDS_SOURCE_PNG, output, &profile, never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);

    /* BGRA, in two levels. */
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.pixel_format == EDDS_PIXEL_BGRA8 && info.mip_count == 2);

    /* Level 0: the PNG's samples. */
    CHECK(edds_preview(output, &info, 0, never_cancelled, NULL, &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == sizeof expected_mip_zero && memcmp(rgba, expected_mip_zero, sizeof expected_mip_zero) == 0);
    edds_free(rgba);
    rgba = NULL;

    /* Level 1: their average. */
    CHECK(edds_preview(output, &info, 1, never_cancelled, NULL, &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == sizeof expected_mip_one && memcmp(rgba, expected_mip_one, sizeof expected_mip_one) == 0);

    edds_free(rgba);
    fclose(source);
    fclose(output);
    fixture_free(png);

    return 1;
}

/**
 * The 2x1 RGB PNG with a suggested palette and a tRNS key that matches its first pixel, converted
 * without mips: BGRA, the first pixel transparent and the second opaque, both keeping their colour.
 */
static int png_colour_key_is_transparency_and_a_palette_changes_nothing(void) {
    /* The RGBA the level must hold. */
    static const uint8_t expected[] = { 10, 20, 30, 0, 40, 50, 60, 255 };

    /* The PNG source, and the converted file. */
    test_bytes png    = fixture_png_rgb_keyed();
    FILE      *source = stream_of(png.data, png.size);
    FILE      *output = temporary();

    /* The conversion settings, the inspected output, and the error a failed call fills in. */
    edds_profile profile;
    edds_info    info;
    edds_error   error;

    /* The previewed level. */
    uint8_t *rgba      = NULL;
    size_t   rgba_size = 0;

    CHECK(png.data != NULL && source != NULL && output != NULL);

    /* Converted without mips, stored as COPY. */
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    profile.generate_mips   = 0;
    CHECK(edds_convert(source, EDDS_SOURCE_PNG, output, &profile, never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);

    /* BGRA, previewed as the expected RGBA. */
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.pixel_format == EDDS_PIXEL_BGRA8);
    CHECK(edds_preview(output, &info, 0, never_cancelled, NULL, &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == sizeof expected && memcmp(rgba, expected, sizeof expected) == 0);

    edds_free(rgba);
    fclose(source);
    fclose(output);
    fixture_free(png);

    return 1;
}

/**
 * The 3x2 RGBA PNG with a gAMA chunk added, converted without mips: its first pixel comes out as
 * the PNG stores it.
 */
static int png_gamma_is_metadata_not_a_sample_transform(void) {
    /* The PNG's first pixel. */
    static const uint8_t expected_first_pixel[] = { 10, 20, 30, 40 };

    /* The PNG source, and the converted file. */
    test_bytes png    = fixture_png_rgba_gamma();
    FILE      *source = stream_of(png.data, png.size);
    FILE      *output = temporary();

    /* The conversion settings, the inspected output, and the error a failed call fills in. */
    edds_profile profile;
    edds_info    info;
    edds_error   error;

    /* The previewed level. */
    uint8_t *rgba      = NULL;
    size_t   rgba_size = 0;

    CHECK(png.data != NULL && source != NULL && output != NULL);

    /* Converted without mips, stored as COPY. */
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    profile.generate_mips   = 0;
    CHECK(edds_convert(source, EDDS_SOURCE_PNG, output, &profile, never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);

    /* The whole 3x2 level previewed, its first pixel unchanged. */
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(edds_preview(output, &info, 0, never_cancelled, NULL, &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == 24u && memcmp(rgba, expected_first_pixel, sizeof expected_first_pixel) == 0);

    edds_free(rgba);
    fclose(source);
    fclose(output);
    fixture_free(png);

    return 1;
}

/**
 * A 2x2 32-bit TGA whose descriptor, 0x18, stores the rows bottom first and each row right to
 * left, with eight alpha bits. Converted without mips, it comes out BGRA and previews top-left
 * pixel first, with its alpha.
 */
static int tga_origin_channels_and_declared_alpha_are_normalized(void) {
    /* The TGA: an 18-byte header, then the BGRA samples, from the bottom-right pixel. */
    static const uint8_t tga[] = {
        0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        2, 0, 2, 0, 32, 0x18,
        12, 11, 10, 13, 9, 8, 7, 10,
        6, 5, 4, 7, 3, 2, 1, 4
    };

    /* The RGBA the level must hold, from the top-left pixel. */
    static const uint8_t expected[] = {
        1, 2, 3, 4, 4, 5, 6, 7,
        7, 8, 9, 10, 10, 11, 12, 13
    };

    /* The source, and the converted file. */
    FILE *source = stream_of(tga, sizeof tga);
    FILE *output = temporary();

    /* The conversion settings, the inspected output, and the error a failed call fills in. */
    edds_profile profile;
    edds_info    info;
    edds_error   error;

    /* The previewed level. */
    uint8_t *rgba      = NULL;
    size_t   rgba_size = 0;

    CHECK(source != NULL && output != NULL);

    /* Converted without mips, stored as COPY. */
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    profile.generate_mips   = 0;
    CHECK(edds_convert(source, EDDS_SOURCE_TGA, output, &profile, never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);

    /* BGRA, previewed as the expected RGBA. */
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.pixel_format == EDDS_PIXEL_BGRA8);
    CHECK(edds_preview(output, &info, 0, never_cancelled, NULL, &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == sizeof expected && memcmp(rgba, expected, sizeof expected) == 0);

    edds_free(rgba);
    fclose(source);
    fclose(output);

    return 1;
}

/**
 * A flat 25x1 TGA converted without mips. At a threshold of 100 every compression mode stores the
 * level as LZ4, and it previews unchanged. With FASTEST, the level stays LZ4 at the threshold equal
 * to its stored size as a percentage of the 100 decoded bytes, and is stored as COPY one below
 * that and at 0.
 */
static int format_compress_selects_lossless_lz4_only_at_the_threshold(void) {
    enum {
        WIDTH     = 25,
        TGA_BYTES = 18 + WIDTH * 3
    };

    uint8_t      tga[TGA_BYTES] = { 0 };
    edds_profile profile;
    uint32_t     fastest_stored = 0;

    /* The TGA header: uncompressed true colour, WIDTH x 1, 24 bits, top row first. */
    tga[2]  = 2;
    tga[12] = WIDTH;
    tga[14] = 1;
    tga[16] = 24;
    tga[17] = 0x20;

    /* Every pixel the same colour. */
    for (size_t at = 18; at < sizeof tga; at += 3u) {
        tga[at]      = 30;
        tga[at + 1u] = 20;
        tga[at + 2u] = 10;
    }

    /* The default profile, without mips. */
    edds_default_profile(&profile);
    profile.generate_mips = 0;

    /* Each compression mode in turn, at a threshold of 100. */
    for (edds_format_compress mode = EDDS_COMPRESS_FASTEST; mode <= EDDS_COMPRESS_BEST; mode = (edds_format_compress)(mode + 1)) {
        FILE      *source = stream_of(tga, sizeof tga);
        FILE      *output = temporary();
        edds_info  info;
        edds_error error;
        uint8_t   *rgba      = NULL;
        size_t     rgba_size = 0;

        CHECK(source != NULL && output != NULL);
        profile.format_compress    = mode;
        profile.compress_threshold = 100;
        CHECK(edds_convert(source, EDDS_SOURCE_TGA, output, &profile, never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);

        /* Stored as LZ4; FASTEST's stored size is kept for the thresholds below. */
        CHECK(fseek(output, 0, SEEK_SET) == 0);
        CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
        CHECK(info.mips[0].container == EDDS_CONTAINER_LZ4);

        if (mode == EDDS_COMPRESS_FASTEST) {
            fastest_stored = info.mips[0].stored_bytes;
        }

        /* Every pixel previews as the first one: red 10, green 20, blue 30, opaque. */
        CHECK(edds_preview(output, &info, 0, never_cancelled, NULL, &rgba, &rgba_size, &error) == EDDS_OK);
        CHECK(rgba_size == WIDTH * 4u &&
            rgba[0] == 10 &&
            rgba[1] == 20 &&
            rgba[2] == 30 &&
            rgba[3] == 255 &&
            memcmp(rgba, rgba + 4, rgba_size - 4u) == 0);

        edds_free(rgba);
        fclose(source);
        fclose(output);
    }

    /* FASTEST's stored size, as a whole percentage of the level's 100 decoded bytes. */
    CHECK(fastest_stored > 0 && (fastest_stored * 100u) % (WIDTH * 4u) == 0);

    {
        const uint32_t equality = fastest_stored * 100u / (WIDTH * 4u);

        /* With that percentage as the threshold the level stays LZ4; one below, it is COPY. */
        for (unsigned below = 0; below < 2u; ++below) {
            FILE      *source = stream_of(tga, sizeof tga);
            FILE      *output = temporary();
            edds_info  info;
            edds_error error;

            CHECK(source != NULL && output != NULL && equality > 0u);
            profile.format_compress    = EDDS_COMPRESS_FASTEST;
            profile.compress_threshold = equality - below;
            CHECK(edds_convert(source, EDDS_SOURCE_TGA, output, &profile, never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);
            CHECK(fseek(output, 0, SEEK_SET) == 0);
            CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
            CHECK(info.mips[0].container == (below == 0u ? EDDS_CONTAINER_LZ4 : EDDS_CONTAINER_COPY));
            fclose(source);
            fclose(output);
        }
    }

    /* At a threshold of 0 the level is stored as COPY. */
    {
        FILE      *source = stream_of(tga, sizeof tga);
        FILE      *output = temporary();
        edds_info  info;
        edds_error error;

        CHECK(source != NULL && output != NULL);
        profile.format_compress    = EDDS_COMPRESS_FASTEST;
        profile.compress_threshold = 0;
        CHECK(edds_convert(source, EDDS_SOURCE_TGA, output, &profile, never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);
        CHECK(fseek(output, 0, SEEK_SET) == 0);
        CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
        CHECK(info.mips[0].container == EDDS_CONTAINER_COPY);
        fclose(source);
        fclose(output);
    }

    return 1;
}

/**
 * A 117x1 32-bit TGA built from a 128-byte pattern, with short and medium partial copies of it in
 * between. Converted without mips at a threshold of 100, every compression mode stores LZ4 that
 * decodes to the whole level, and FASTEST stores more than MEDIUM, which stores more than BEST.
 */
static int compression_efforts_are_observably_distinct_and_lossless(void) {
    enum {
        PATTERN     = 128,
        SHORT       = 8,
        MEDIUM      = 52,
        PIXEL_BYTES = 468
    };

    uint8_t  tga[18 + PIXEL_BYTES];
    uint8_t  pattern[PATTERN];
    uint32_t stored[3] = { 0, 0, 0 };
    size_t   at        = 18;

    /* The header: uncompressed true colour, 117x1, 32 bits, top row first, eight alpha bits. */
    memset(tga, 0, sizeof tga);
    tga[2]  = 2;
    tga[12] = (uint8_t)(PIXEL_BYTES / 4);
    tga[14] = 1;
    tga[16] = 32;
    tga[17] = 0x28;

    /* The pattern. */
    for (size_t index = 0; index < PATTERN; ++index) {
        pattern[index] = (uint8_t)((index * 37u + index * index * 13u) % 251u);
    }

    /* The pixels: first the whole pattern. */
    memcpy(tga + at, pattern, PATTERN);
    at += PATTERN;

    /* Twelve decoys: the pattern's first 4 bytes, then 4 bytes of one value. */
    for (unsigned decoy = 0; decoy < 12u; ++decoy) {
        memcpy(tga + at, pattern, 4);
        memset(tga + at + 4u, (int)(decoy + 1u), SHORT - 4u);
        at += SHORT;
    }

    /* The pattern's first 48 bytes, then 4 bytes of 0xfe. */
    memcpy(tga + at, pattern, MEDIUM - 4u);
    memset(tga + at + MEDIUM - 4u, 0xfe, 4u);
    at += MEDIUM;

    /* Eight more decoys, with values of their own. */
    for (unsigned decoy = 0; decoy < 8u; ++decoy) {
        memcpy(tga + at, pattern, 4);
        memset(tga + at + 4u, (int)(decoy + 101u), SHORT - 4u);
        at += SHORT;
    }

    /* The whole pattern again, which ends the file exactly. */
    memcpy(tga + at, pattern, PATTERN);
    at += PATTERN;
    CHECK(at == sizeof tga);

    for (edds_format_compress mode = EDDS_COMPRESS_FASTEST; mode <= EDDS_COMPRESS_BEST; mode = (edds_format_compress)(mode + 1)) {
        /* The source, and the converted file. */
        FILE *source = stream_of(tga, sizeof tga);
        FILE *output = temporary();

        /* The conversion settings, the inspected output, and the error a failed call fills in. */
        edds_profile profile;
        edds_info    info;
        edds_error   error;

        /* The previewed level. */
        uint8_t *rgba      = NULL;
        size_t   rgba_size = 0;

        CHECK(source != NULL && output != NULL);

        /* Converted without mips, in this mode, at a threshold of 100. */
        edds_default_profile(&profile);
        profile.generate_mips      = 0;
        profile.format_compress    = mode;
        profile.compress_threshold = 100;
        CHECK(edds_convert(source, EDDS_SOURCE_TGA, output, &profile, never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);

        /* Stored as LZ4; the stored size is kept for the comparison below. */
        CHECK(fseek(output, 0, SEEK_SET) == 0);
        CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
        CHECK(info.mips[0].container == EDDS_CONTAINER_LZ4);
        stored[mode - EDDS_COMPRESS_FASTEST] = info.mips[0].stored_bytes;

        /* Decoded whole. */
        CHECK(edds_preview(output, &info, 0, never_cancelled, NULL, &rgba, &rgba_size, &error) == EDDS_OK);
        CHECK(rgba_size == PIXEL_BYTES);

        edds_free(rgba);
        fclose(source);
        fclose(output);
    }

    CHECK(stored[0] > stored[1] && stored[1] > stored[2]);

    return 1;
}

/**
 * A .meta text with a comment, unknown fields and an override in its XBOX_ONE configuration is
 * parsed, written and parsed again. The text written is the canonical one: no comment, no unknown
 * field, no override, and empty XBOX_ONE, PS4 and LINUX configurations under PC. The second parse
 * gives exactly the first one's metadata.
 */
static int metadata_round_trip_is_canonical_and_preserves_identity(void) {
    /* The text to parse: a comment, unknown fields, and an override in XBOX_ONE. */
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

    /* The text the writer must produce from it. */
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

    /* The text as a file, and the file the metadata is written to. */
    FILE *input  = stream_of((const uint8_t *)source, strlen(source));
    FILE *output = temporary();

    /* The first parse, the second, and the error a failed call fills in. */
    edds_metadata metadata;
    edds_metadata reparsed;
    edds_error    error;

    /* The text written, read back. */
    char actual[sizeof expected];

    CHECK(input != NULL && output != NULL);

    /* Parsed: the GUID and the path out of Name, the source file and the PC settings. */
    CHECK(edds_metadata_parse(input, &metadata, &error) == EDDS_OK);
    CHECK(strcmp(metadata.guid, "a1B2c3D4e5F60718") == 0);
    CHECK(strcmp(metadata.name, "Probe/Images/pixel.edds") == 0);
    CHECK(strcmp(metadata.source_file, "pixel.png") == 0);
    CHECK(metadata.source_format == EDDS_SOURCE_PNG);
    CHECK(metadata.profile.format_compress == EDDS_COMPRESS_BEST);
    CHECK(metadata.profile.compress_threshold == 73 && metadata.profile.remove_mips == 2u);
    CHECK(!metadata.profile.contains_mips && !metadata.profile.generate_mips);
    CHECK(metadata.profile.normalize && metadata.profile.mipmap_filter == EDDS_FILTER_BOX);

    /* Written as exactly the expected text. */
    CHECK(edds_metadata_write(output, &metadata, &error) == EDDS_OK);
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(fread(actual, 1, sizeof expected - 1u, output) == sizeof expected - 1u);
    actual[sizeof expected - 1u] = '\0';
    CHECK(strcmp(actual, expected) == 0);

    /* Parsed again, to the same metadata. */
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_metadata_parse(output, &reparsed, &error) == EDDS_OK);
    CHECK(memcmp(&metadata, &reparsed, sizeof metadata) == 0);

    fclose(input);
    fclose(output);

    return 1;
}

/**
 * A minimal .meta text for a TGA, with one known but unsupported setting or combination added:
 * every parse is refused as unsupported, with the error code its case names.
 */
static int known_but_unsupported_metadata_is_never_defaulted(void) {
    /* The settings added, and the error code each must be refused with. */
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
        char          source[512];
        edds_metadata metadata;
        edds_error    error;
        FILE         *input;

        /* The text: the PC configuration of a TGA source, with the case's setting in it. */
        (void)snprintf(source, sizeof source,
            "MetaFileClass { Name \"{0123456789ABCDEF}a.edds\" Configurations { "
            "TGAResourceClass PC { SourceFile \"a.tga\" %s } } }",
            cases[at].setting);

        input = stream_of((const uint8_t *)source, strlen(source));
        CHECK(input != NULL);
        CHECK(edds_metadata_parse(input, &metadata, &error) == EDDS_UNSUPPORTED_FORMAT);
        CHECK(strcmp(error.code, cases[at].code) == 0);
        fclose(input);
    }

    return 1;
}

/**
 * Every registered resource class parses back to its own format, and only to its own.
 * For each of the five source formats, metadata under the default profile (supplied mips for a
 * DDS) is written; the text must name the format's resource class and parse back to the same
 * format and source file.
 */
static int every_resource_class_round_trips_through_metadata(void) {
    size_t                        count        = 0;
    const edds_source_capability *capabilities = edds_source_capabilities(&count);

    CHECK(count == 5u);

    for (size_t at = 0; at < count; ++at) {
        /* The text written, the file it is written to, and the file it is parsed back from. */
        char  text[768];
        FILE *written = temporary();
        FILE *reread;

        /* The metadata written and parsed back, a failed call's error, and the text's length. */
        edds_metadata metadata;
        edds_metadata parsed;
        edds_error    error;
        long          size;

        /* Metadata for a source of this format, under the default profile. */
        memset(&metadata, 0, sizeof metadata);
        memcpy(metadata.guid, "0123456789ABCDEF", 17);
        (void)snprintf(metadata.name, sizeof metadata.name, "Probe/pixel.edds");
        (void)snprintf(metadata.source_file, sizeof metadata.source_file, "pixel%s", capabilities[at].extension);
        metadata.source_format = capabilities[at].format;
        edds_default_profile(&metadata.profile);

        /* A DDS supplies its own mips. */
        if (metadata.source_format == EDDS_SOURCE_DDS) {
            metadata.profile.contains_mips = 1;
            metadata.profile.generate_mips = 0;
        }

        /* Written, and read back as text that names the resource class. */
        CHECK(written != NULL);
        CHECK(edds_metadata_write(written, &metadata, &error) == EDDS_OK);
        CHECK(fseek(written, 0, SEEK_END) == 0 && (size = ftell(written)) > 0);
        CHECK((size_t)size < sizeof text && fseek(written, 0, SEEK_SET) == 0);
        CHECK(fread(text, 1, (size_t)size, written) == (size_t)size);
        fclose(written);
        CHECK(strstr(text, capabilities[at].resource_class) != NULL);

        /* Parsed back: the same format and source file. */
        reread = stream_of((const uint8_t *)text, (size_t)size);
        CHECK(reread != NULL);
        CHECK(edds_metadata_parse(reread, &parsed, &error) == EDDS_OK);
        CHECK(parsed.source_format == capabilities[at].format);
        CHECK(strcmp(parsed.source_file, metadata.source_file) == 0);
        fclose(reread);
    }

    return 1;
}

/**
 * A TGAResourceClass configuration whose source file is a .png: the parse is refused as invalid
 * input, with "source-format-mismatch".
 */
static int metadata_source_class_must_match_its_extension(void) {
    static const char source[] =
        "MetaFileClass { Name \"{0123456789ABCDEF}a.edds\" Configurations { "
        "TGAResourceClass PC { SourceFile \"a.png\" } } }";
    FILE         *input = stream_of((const uint8_t *)source, strlen(source));
    edds_metadata metadata;
    edds_error    error;

    CHECK(input != NULL);
    CHECK(edds_metadata_parse(input, &metadata, &error) == EDDS_INVALID_INPUT);
    CHECK(strcmp(error.code, "source-format-mismatch") == 0);
    fclose(input);

    return 1;
}

/**
 * edds_batch_parse_line on a header, a job and broken lines. The header and the job parse with
 * their fields; a job whose mip settings conflict is refused as unsupported; every prefix of the
 * job, a protocol version of 2, an unknown kind and a line over the size limit are refused as
 * invalid, the last three each with its own error code.
 */
static int batch_ndjson_parser_has_a_bounded_mutation_corpus(void) {
    /* A batch header announcing 100 jobs. */
    static const char header[] = "{\"protocolVersion\":1,\"kind\":\"batch\",\"jobCount\":100}";

    /* A job with Normalize set, its mips made by the Normalize function, two levels removed. */
    static const char job[] =
        "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"a\","
        "\"input\":\"C:\\\\a.png\",\"output\":\"C:\\\\a.edds\","
        "\"metadata\":null,\"identity\":null,\"profile\":{"
        "\"TargetFormat\":\"EnfusionDDS\",\"FormatCompress\":\"Fastest\","
        "\"CompressTreshold\":80,\"RemoveMips\":2,\"Conversion\":\"None\","
        "\"ConversionQuality\":1,\"Swizzling\":\"None\",\"ContainsMips\":false,"
        "\"GenerateMips\":true,\"Normalize\":true,\"MipMapFunction\":\"Normalize\","
        "\"MipMapFilter\":\"Box\",\"TiledTexture\":true},\"expected\":null}";

    /* A job that generates no mips but sets the Kaiser filter. */
    static const char inactive_mip_setting[] =
        "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"a\","
        "\"input\":\"C:\\\\a.png\",\"output\":\"C:\\\\a.edds\","
        "\"metadata\":null,\"identity\":null,\"profile\":{"
        "\"TargetFormat\":\"EnfusionDDS\",\"FormatCompress\":\"Fastest\","
        "\"CompressTreshold\":80,\"RemoveMips\":0,\"Conversion\":\"None\","
        "\"ConversionQuality\":1,\"Swizzling\":\"None\",\"ContainsMips\":false,"
        "\"GenerateMips\":false,\"Normalize\":false,\"MipMapFunction\":\"Filter\","
        "\"MipMapFilter\":\"Kaiser\",\"TiledTexture\":true},\"expected\":null}";

    /* A header of protocol version 2, and a record of an unknown kind. */
    static const char incompatible[] = "{\"protocolVersion\":2,\"kind\":\"batch\",\"jobCount\":1}";
    static const char unknown[]      = "{\"protocolVersion\":1,\"kind\":\"surprise\"}";

    edds_batch_record record;
    edds_error        error;

    /* The header, and the job with its settings. */
    CHECK(edds_batch_parse_line(header, strlen(header), &record, &error) == EDDS_OK);
    CHECK(record.kind == EDDS_BATCH_HEADER && record.job_count == 100u);
    CHECK(edds_batch_parse_line(job, strlen(job), &record, &error) == EDDS_OK);
    CHECK(record.kind == EDDS_BATCH_JOB && strcmp(record.job.id, "a") == 0);
    CHECK(record.job.profile.remove_mips == 2u && record.job.profile.normalize);
    CHECK(record.job.profile.mipmap_function == EDDS_MIPMAP_NORMALIZE);
    CHECK(record.job.profile.mipmap_filter == EDDS_FILTER_BOX);

    /* The Kaiser filter without generated mips: a conflict. */
    CHECK(edds_batch_parse_line(inactive_mip_setting, strlen(inactive_mip_setting), &record, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-combination") == 0);

    /* Every prefix of the job. */
    for (size_t size = 0; size < strlen(job); ++size) {
        CHECK(edds_batch_parse_line(job, size, &record, &error) == EDDS_INVALID_INVOCATION);
    }

    /* The wrong protocol version, an unknown kind, and a line one byte over the size limit. */
    CHECK(edds_batch_parse_line(incompatible, strlen(incompatible), &record, &error) == EDDS_INVALID_INVOCATION);
    CHECK(strcmp(error.code, "incompatible-batch-version") == 0);
    CHECK(edds_batch_parse_line(unknown, strlen(unknown), &record, &error) == EDDS_INVALID_INVOCATION);
    CHECK(strcmp(error.code, "unknown-batch-record") == 0);
    CHECK(edds_batch_parse_line("{}", EDDS_BATCH_MAX_LINE_BYTES + 1u, &record, &error) == EDDS_INVALID_INVOCATION);
    CHECK(strcmp(error.code, "batch-line-size") == 0);

    return 1;
}

/**
 * Four lines, the second blank, ended by LF, CRLF, CRLF and nothing, pushed to the batch reader
 * in chunks of every size from one byte to the whole stream.
 */
static int batch_reader_frames_lines_however_a_pipe_split_them(void) {
    static const char stream[] = "{\"a\":1}\n\r\n{\"b\":2}\r\n{\"c\":3}";
    const size_t      total    = sizeof stream - 1u;

    /* Every split of the same bytes has to frame the same four lines, the blank one included. */
    for (size_t chunk = 1u; chunk <= total; ++chunk) {
        edds_batch_reader reader;

        /* The lines framed so far, how many there are, and how much of the stream was pushed. */
        char   framed[8][64];
        size_t lines = 0;
        size_t at    = 0;

        edds_batch_reader_init(&reader);

        /* The stream pushed a chunk at a time; every line made ready is copied out. */
        while (at < total) {
            size_t      remaining = total - at < chunk ? total - at : chunk;
            const char *data      = stream + at;

            at += remaining;

            /* A push takes bytes up to the first line they complete, so the rest goes in again. */
            while (remaining > 0u) {
                size_t consumed = 0;
                size_t size     = 0;

                const edds_batch_line line = edds_batch_reader_push(&reader, data, remaining, &consumed, &size);

                data      += consumed;
                remaining -= consumed;

                if (line != EDDS_BATCH_LINE_READY) {
                    continue;
                }

                CHECK(lines < 8u && size < sizeof framed[0]);
                memcpy(framed[lines++], reader.line, size + 1u);
            }
        }

        /* The last line has no newline: finishing the stream frames it. */
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

/**
 * A line of EDDS_BATCH_MAX_LINE_BYTES + 16 bytes, then a "{}" line, in one push: the long line is
 * reported as an overflow, and the next push frames the short line whole.
 */
static int batch_reader_refuses_an_oversized_line_without_framing_half_of_it(void) {
    /* The reader, and the size of the bytes pushed into it. */
    const size_t      span = EDDS_BATCH_MAX_LINE_BYTES + 20u;
    edds_batch_reader reader;

    /* The bytes, and what a push reports: the bytes it took, the line's length, the result. */
    char           *oversized = malloc(span);
    size_t          consumed  = 0;
    size_t          size      = 0;
    edds_batch_line line;

    CHECK(oversized != NULL);

    /* A line of 'x' over the limit, then "{}", each ended by a newline. */
    memset(oversized, 'x', EDDS_BATCH_MAX_LINE_BYTES + 16u);
    oversized[EDDS_BATCH_MAX_LINE_BYTES + 16u] = '\n';
    memcpy(oversized + EDDS_BATCH_MAX_LINE_BYTES + 17u, "{}\n", 3u);

    /* The first push stops after the long line's newline and reports the overflow. */
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

/**
 * What the probe tasks record: how many times each index ran, the number of workers the pool
 * reports, and how many tasks are running now and at most.
 */
typedef struct pool_probe {
    unsigned long ran[512];
    uint32_t      workers;
    uint32_t      live;
    uint32_t      peak;
} pool_probe;

/**
 * One probe task: holds 3 MiB of the budget while it counts its run and itself among the running
 * tasks, then counts itself out and gives the budget back.
 */
static void pool_probe_task(void *context, uint32_t index, edds_pool *pool) {
    pool_probe    *probe  = (pool_probe *)context;
    const uint64_t charge = 3u * 1024u * 1024u;

    edds_pool_reserve(pool, charge);

    /* Under the output lock: the worker count, this run, and the running tasks with their peak. */
    edds_pool_lock_output(pool);
    probe->workers     = edds_pool_workers(pool);
    probe->ran[index] += 1ul;

    if (++probe->live > probe->peak) {
        probe->peak = probe->live;
    }

    edds_pool_unlock_output(pool);

    /* Counted out, under the lock again. */
    edds_pool_lock_output(pool);
    --probe->live;
    edds_pool_unlock_output(pool);

    edds_pool_release(pool, charge);
}

/**
 * 256 probe tasks over the pool with an 8 MiB budget: each index runs exactly once and no other
 * does, at most two run at once, and the pool starts between 1 and EDDS_POOL_MAX_WORKERS workers.
 * A run of no tasks succeeds and runs nothing.
 */
static int the_pool_runs_every_job_once_inside_one_bounded_budget(void) {
    pool_probe probe;
    edds_error error;

    memset(&probe, 0, sizeof probe);
    CHECK(edds_pool_run(256u, 8u * 1024u * 1024u, pool_probe_task, &probe, &error) == EDDS_OK);

    /* Each of the 256 tasks ran exactly once, and no index past them ran at all. */
    for (uint32_t at = 0; at < 256u; ++at) {
        CHECK(probe.ran[at] == 1ul);
    }

    for (uint32_t at = 256u; at < 512u; ++at) {
        CHECK(probe.ran[at] == 0ul);
    }

    /* Eight megabytes of budget admit two three-megabyte images at a time, never a third. */
    CHECK(probe.peak <= 2u);
    CHECK(probe.workers >= 1u && probe.workers <= EDDS_POOL_MAX_WORKERS);

    /* One task gets one worker, and no tasks still get at least one. */
    CHECK(edds_pool_worker_count(1u) == 1u);
    CHECK(edds_pool_worker_count(0u) >= 1u);

    /* A run of no tasks. */
    memset(&probe, 0, sizeof probe);
    CHECK(edds_pool_run(0u, 0u, pool_probe_task, &probe, &error) == EDDS_OK);
    CHECK(probe.ran[0] == 0ul);

    return 1;
}

/**
 * Eight probe tasks of 3 MiB each over a 1 MiB budget: every one runs, one at a time. The pool's
 * first charge for a source is above zero, larger for a larger source, and UINT64_MAX for the
 * largest.
 */
static int an_image_larger_than_the_whole_budget_still_runs_alone(void) {
    pool_probe probe;
    edds_error error;

    memset(&probe, 0, sizeof probe);

    /* Three megabytes charged against a budget of one: refusing it would strand the image. */
    CHECK(edds_pool_run(8u, 1024u * 1024u, pool_probe_task, &probe, &error) == EDDS_OK);

    for (uint32_t at = 0; at < 8u; ++at) {
        CHECK(probe.ran[at] == 1ul);
    }

    CHECK(probe.peak == 1u);

    /* The pool's first charge for a source of no bytes, of 1024, and of the largest size. */
    CHECK(edds_pool_charge_of(0u) > 0u);
    CHECK(edds_pool_charge_of(1024u) > edds_pool_charge_of(0u));
    CHECK(edds_pool_charge_of(UINT64_MAX) == UINT64_MAX);

    return 1;
}

/**
 * A probe task whose charge depends on its index, 4 MiB for every fourth and 256 KiB for the
 * rest: it counts its run and the running tasks as pool_probe_task does.
 */
static void pool_mixed_task(void *context, uint32_t index, edds_pool *pool) {
    pool_probe *probe = (pool_probe *)context;

    /* Every fourth image needs the whole budget; the rest are small enough to share it. */
    const uint64_t charge = index % 4u == 0u ? 4u * 1024u * 1024u : 256u * 1024u;

    edds_pool_reserve(pool, charge);

    /* Under the output lock: this run, and the running tasks with their peak. */
    edds_pool_lock_output(pool);
    probe->ran[index] += 1ul;

    if (++probe->live > probe->peak) {
        probe->peak = probe->live;
    }

    edds_pool_unlock_output(pool);

    /* Counted out, under the lock again. */
    edds_pool_lock_output(pool);
    --probe->live;
    edds_pool_unlock_output(pool);

    edds_pool_release(pool, charge);
}

/** 128 mixed tasks over a 4 MiB budget, every fourth wanting all of it: each runs exactly once. */
static int oversized_and_small_images_share_one_budget_without_starving(void) {
    pool_probe probe;
    edds_error error;

    memset(&probe, 0, sizeof probe);

    /* Images wanting everything and images wanting a little, interleaved: all of them finish. */
    CHECK(edds_pool_run(128u, 4u * 1024u * 1024u, pool_mixed_task, &probe, &error) == EDDS_OK);

    for (uint32_t at = 0; at < 128u; ++at) {
        CHECK(probe.ran[at] == 1ul);
    }

    return 1;
}

/*
 * The proven JPG and TIFF subtype matrix, stated as tests. Every subtype the decoders accept is
 * here with the pixels it owes, and every subtype they refuse is here with the refusal it owes.
 */

/**
 * Converts one source through the public seam and hands back its level-zero RGBA.
 * The profile is the default one, its levels stored as COPY and generate_mips as given. Returns 1
 * with info filled and *rgba for the caller to free with edds_free, or 0 when any step fails.
 */
static int decoded_through_convert(
    const uint8_t     *bytes,
    size_t             size,
    edds_source_format format,
    int                generate_mips,
    edds_info         *info,
    uint8_t          **rgba,
    size_t            *rgba_size) {
    FILE        *source = stream_of(bytes, size);
    FILE        *output = temporary();
    edds_profile profile;
    edds_error   error;
    int          ok;

    if (source == NULL || output == NULL) {
        if (source != NULL) {
            fclose(source);
        }

        if (output != NULL) {
            fclose(output);
        }

        return 0;
    }

    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    profile.generate_mips   = generate_mips;

    /* Converted, inspected and previewed, each step only if the one before succeeded. */
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
 * Runs a source that has to be refused. Returns the refusal, or EDDS_OK (which no refusal ever
 * is) when the conversion either succeeded or left bytes behind it.
 */
static edds_status refused_by_profile(
    const uint8_t      *bytes,
    size_t              size,
    edds_source_format  format,
    const edds_profile *profile,
    edds_error         *error) {
    FILE       *source = stream_of(bytes, size);
    FILE       *output = temporary();
    edds_status status;
    long        written = -1;

    if (source == NULL || output == NULL) {
        if (source != NULL) {
            fclose(source);
        }

        if (output != NULL) {
            fclose(output);
        }

        return EDDS_OK;
    }

    status = edds_convert(source, format, output, profile, never_cancelled, NULL, NULL, NULL, error);

    /* How many bytes the conversion left in the output. */
    if (fseek(output, 0, SEEK_END) == 0) {
        written = ftell(output);
    }

    fclose(source);
    fclose(output);

    return status == EDDS_OK || written != 0 ? EDDS_OK : status;
}

/** refused_by_profile under the default profile. */
static edds_status refused_by(const uint8_t *bytes, size_t size, edds_source_format format, edds_error *error) {
    edds_profile profile;

    edds_default_profile(&profile);

    return refused_by_profile(bytes, size, format, &profile, error);
}

/**
 * The 4x2 BGRX DDS with only its top level: with generated mips it converts to three levels;
 * claiming supplied mips is refused as "unsupported-dds-mip-layout"; and with a mip count of 0
 * it still converts, its first pixel intact.
 */
static int dds_top_level_generates_but_cannot_claim_a_supplied_chain(void) {
    test_bytes   dds    = fixture_dds_bgrx_top();
    FILE        *source = stream_of(dds.data, dds.size);
    FILE        *output = temporary();
    edds_profile profile;
    edds_info    info;
    edds_error   error;

    CHECK(dds.data != NULL && source != NULL && output != NULL);

    /* Mips generated, stored as COPY: three levels from 4x2 down. */
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    CHECK(edds_convert(source, EDDS_SOURCE_DDS, output, &profile, never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.width == 4u && info.height == 2u && info.mip_count == 3u);
    fclose(source);
    fclose(output);

    /* Mips claimed as supplied by a file that has only its top level. */
    profile.generate_mips = 0;
    profile.contains_mips = 1;
    CHECK(refused_by_profile(dds.data, dds.size, EDDS_SOURCE_DDS, &profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-dds-mip-layout") == 0);

    /* DDS permits zero for a top-only file; keep that admitted header shape controlled too. */
    memset(dds.data + 28, 0, 4);

    {
        uint8_t *rgba      = NULL;
        size_t   rgba_size = 0;

        CHECK(decoded_through_convert(dds.data, dds.size, EDDS_SOURCE_DDS, 0, &info, &rgba, &rgba_size));
        CHECK(rgba_size == 4u * 2u * 4u && rgba[0] == 1u && rgba[1] == 2u && rgba[2] == 3u && rgba[3] == 255u);
        edds_free(rgba);
    }

    fixture_free(dds);

    return 1;
}

/**
 * DDS refusals, each with its own error code and no output left behind. The three-level DDS: mips
 * both supplied and generated, all three levels removed, the file one byte short and every prefix
 * of it, a mip count of 2, a reserved header byte set, a pitch one too large. Then the top-only
 * DDS with one trailing byte, and a PNG that claims to supply mips.
 */
static int dds_refusals_are_precise_and_atomic(void) {
    test_bytes   dds = fixture_dds_bgrx_mips();
    test_bytes   top = fixture_dds_bgrx_top();
    test_bytes   png = fixture_png_rgba();
    edds_profile profile;
    edds_error   error;

    CHECK(dds.data != NULL && top.data != NULL && png.data != NULL && dds.size > 128u);

    /* Mips both supplied and generated. */
    edds_default_profile(&profile);
    profile.contains_mips = 1;
    memset(&error, 0, sizeof error);
    CHECK(refused_by_profile(dds.data, dds.size, EDDS_SOURCE_DDS, &profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-combination") == 0);

    /* The supplied mips, with all three levels removed. */
    profile.generate_mips = 0;
    profile.contains_mips = 1;
    profile.remove_mips   = 3;
    memset(&error, 0, sizeof error);
    CHECK(refused_by_profile(dds.data, dds.size, EDDS_SOURCE_DDS, &profile, &error) == EDDS_INVALID_INPUT);
    CHECK(strcmp(error.code, "remove-mips-out-of-range") == 0);

    /* The file one byte short. */
    profile.remove_mips = 0;
    memset(&error, 0, sizeof error);
    CHECK(refused_by_profile(dds.data, dds.size - 1u, EDDS_SOURCE_DDS, &profile, &error) == EDDS_INVALID_INPUT);
    CHECK(strcmp(error.code, "truncated-dds-mip") == 0);

    /* Every shorter prefix of the file. */
    for (size_t prefix = 0; prefix < dds.size; ++prefix) {
        memset(&error, 0, sizeof error);
        CHECK(refused_by_profile(dds.data, prefix, EDDS_SOURCE_DDS, &profile, &error) != EDDS_OK);
    }

    /* A mip count, at byte 28, of 2 instead of 3; then put back. */
    dds.data[28] = 2u;
    memset(&error, 0, sizeof error);
    CHECK(refused_by_profile(dds.data, dds.size, EDDS_SOURCE_DDS, &profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-dds-mip-layout") == 0);
    dds.data[28] = 3u;

    /* The first reserved header byte, 32, set; then put back. */
    dds.data[32] = 1u;
    memset(&error, 0, sizeof error);
    CHECK(refused_by_profile(dds.data, dds.size, EDDS_SOURCE_DDS, &profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-dds-header") == 0);
    dds.data[32] = 0u;

    /* The pitch, at byte 20, one more than the tight one; then put back. */
    dds.data[20] += 1u;
    memset(&error, 0, sizeof error);
    CHECK(refused_by_profile(dds.data, dds.size, EDDS_SOURCE_DDS, &profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-dds-pitch") == 0);
    dds.data[20] -= 1u;

    /* The top-only DDS taken one byte past its end. */
    ++top.size;
    profile.contains_mips = 0;
    memset(&error, 0, sizeof error);
    CHECK(refused_by_profile(top.data, top.size, EDDS_SOURCE_DDS, &profile, &error) == EDDS_INVALID_INPUT);
    CHECK(strcmp(error.code, "trailing-dds-data") == 0);

    /* A PNG with mips claimed as supplied. */
    profile.contains_mips = 1;
    memset(&error, 0, sizeof error);
    CHECK(refused_by_profile(png.data, png.size, EDDS_SOURCE_PNG, &profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-combination") == 0);

    fixture_free(png);
    fixture_free(top);
    fixture_free(dds);

    return 1;
}

/** Whether every pixel of an RGBA buffer is the given colour and opaque: 1 if so, 0 if not. */
static int every_pixel_is(const uint8_t *rgba, size_t size, uint8_t red, uint8_t green, uint8_t blue) {
    for (size_t at = 0; at + 3u < size; at += 4u) {
        if (rgba[at] != red || rgba[at + 1u] != green || rgba[at + 2u] != blue || rgba[at + 3u] != 255u) {
            return 0;
        }
    }

    return 1;
}

/**
 * The 16x8 YCbCr JPEG fixture, two flat DC-only MCUs, converted without mips: BGRX in one level,
 * grey 78 in the left half and grey 178 in the right, every pixel opaque.
 */
static int baseline_jpeg_carries_two_flat_mcus_without_alpha(void) {
    test_bytes jpeg = fixture_jpeg_ycbcr();
    edds_info  info;
    uint8_t   *rgba      = NULL;
    size_t     rgba_size = 0;

    CHECK(jpeg.data != NULL);
    CHECK(decoded_through_convert(jpeg.data, jpeg.size, EDDS_SOURCE_JPG, 0, &info, &rgba, &rgba_size));
    CHECK(info.width == 16 && info.height == 8 && info.mip_count == 1);

    /* JPEG has no alpha to declare, so the surface is the opaque one. */
    CHECK(info.pixel_format == EDDS_PIXEL_BGRX8);
    CHECK(rgba_size == 16u * 8u * 4u);

    for (size_t pixel = 0; pixel < 16u * 8u; ++pixel) {
        const uint8_t  expected = pixel % 16u < 8u ? 78u : 178u;
        const uint8_t *at       = rgba + pixel * 4u;

        CHECK(at[0] == expected && at[1] == expected && at[2] == expected && at[3] == 255u);
    }

    edds_free(rgba);
    fixture_free(jpeg);

    return 1;
}

/**
 * An 8x8 baseline JPEG with one component, converted without mips: BGRX, every pixel grey 78 and
 * opaque.
 */
static int greyscale_jpeg_decodes_as_one_component(void) {
    /* The entropy-coded data of its one block. */
    static const uint8_t entropy[] = { 0x8d, 0xef };

    /* The JPEG: 8x8, one component. */
    const fixture_jpeg_spec spec = {
        .frame_marker    = 0xc0,
        .precision       = 8,
        .width           = 8,
        .height          = 8,
        .component_count = 1,
        .luma_sampling   = 0x11,
        .entropy         = entropy,
        .entropy_size    = sizeof entropy
    };

    /* The JPEG file, and its size: 0 if it could not be built. */
    uint8_t      file[512];
    const size_t size = fixture_jpeg_build(file, sizeof file, &spec);

    /* The output's inspection, and its top level as RGBA. */
    edds_info info;
    uint8_t  *rgba      = NULL;
    size_t    rgba_size = 0;

    CHECK(size != 0);
    CHECK(decoded_through_convert(file, size, EDDS_SOURCE_JPG, 0, &info, &rgba, &rgba_size));
    CHECK(info.width == 8 && info.height == 8 && info.pixel_format == EDDS_PIXEL_BGRX8);
    CHECK(rgba_size == 8u * 8u * 4u && every_pixel_is(rgba, rgba_size, 78, 78, 78));

    edds_free(rgba);

    return 1;
}

/**
 * 4:2:0 chroma is replicated across its luma block, and the JFIF matrix places the colour.
 * A 16x16 three-component JPEG with its luma sampled 0x22, converted without mips: every pixel is
 * red 78, green 95, blue 0, opaque.
 */
static int subsampled_jpeg_upsamples_chroma_over_its_luma_block(void) {
    /* The entropy-coded data of its one MCU. */
    static const uint8_t entropy[] = { 0x8d, 0xe0, 0x23, 0x78 };

    /* The JPEG: 16x16, three components, luma sampled 0x22. */
    const fixture_jpeg_spec spec = {
        .frame_marker    = 0xc0,
        .precision       = 8,
        .width           = 16,
        .height          = 16,
        .component_count = 3,
        .luma_sampling   = 0x22,
        .entropy         = entropy,
        .entropy_size    = sizeof entropy
    };

    /* The JPEG file, and its size: 0 if it could not be built. */
    uint8_t      file[512];
    const size_t size = fixture_jpeg_build(file, sizeof file, &spec);

    /* The output's inspection, and its top level as RGBA. */
    edds_info info;
    uint8_t  *rgba      = NULL;
    size_t    rgba_size = 0;

    CHECK(size != 0);
    CHECK(decoded_through_convert(file, size, EDDS_SOURCE_JPG, 0, &info, &rgba, &rgba_size));
    CHECK(info.width == 16 && info.height == 16);
    CHECK(rgba_size == 16u * 16u * 4u);
    CHECK(every_pixel_is(rgba, rgba_size, 78, 95, 0));

    edds_free(rgba);

    return 1;
}

/**
 * A 16x8 three-component JPEG with a restart interval of one MCU and a restart marker between its
 * two MCUs, converted without mips: red is 78 in the left half and 178 in the right.
 */
static int jpeg_restart_markers_reset_the_dc_prediction(void) {
    /* The entropy-coded data: the first MCU, the marker 0xFF 0xD0, the second MCU. */
    static const uint8_t entropy[] = { 0x8d, 0xe0, 0xff, 0xd0, 0xb2, 0x00 };

    /* The JPEG: 16x8, three components, a restart interval of one MCU. */
    const fixture_jpeg_spec spec = {
        .frame_marker     = 0xc0,
        .precision        = 8,
        .width            = 16,
        .height           = 8,
        .component_count  = 3,
        .luma_sampling    = 0x11,
        .restart_interval = 1,
        .entropy          = entropy,
        .entropy_size     = sizeof entropy
    };

    /* The JPEG file, and its size: 0 if it could not be built. */
    uint8_t      file[512];
    const size_t size = fixture_jpeg_build(file, sizeof file, &spec);

    /* The output's inspection, and its top level as RGBA. */
    edds_info info;
    uint8_t  *rgba      = NULL;
    size_t    rgba_size = 0;

    CHECK(size != 0);
    CHECK(decoded_through_convert(file, size, EDDS_SOURCE_JPG, 0, &info, &rgba, &rgba_size));
    CHECK(rgba_size == 16u * 8u * 4u);

    for (size_t pixel = 0; pixel < 16u * 8u; ++pixel) {
        CHECK(rgba[pixel * 4u] == (pixel % 16u < 8u ? 78u : 178u));
    }

    edds_free(rgba);

    return 1;
}

/**
 * Six JPEGs, each one change away from a supported one: an SOF2 or SOF9 frame, 12-bit samples, a
 * luma sampling of 0x41, one component sampled 0x22, an EXIF orientation of 6. Each is refused as
 * unsupported, with its own error code, and leaves no output.
 */
static int unsupported_jpeg_subtypes_are_refused_before_any_pixel(void) {
    /* The entropy-coded data every case carries. */
    static const uint8_t entropy[] = { 0x8d, 0xe0, 0xf2, 0x00, 0x7f };

    /* An EXIF block of one tag, 274, the orientation, set to 6. */
    static const uint8_t exif_rotated[] = {
        'E', 'x', 'i', 'f', 0, 0,
        'I', 'I', 0x2a, 0, 8, 0, 0, 0,
        1, 0,
        0x12, 0x01, 3, 0, 1, 0, 0, 0, 6, 0, 0, 0,
        0, 0, 0, 0
    };

    /* Each case: the JPEG, and the error code it must be refused with. */
    const struct {
        fixture_jpeg_spec spec;
        const char       *code;
    } cases[] = {
        /* An SOF2 frame. */
        { { .frame_marker      = 0xc2,
              .precision       = 8,
              .width           = 16,
              .height          = 8,
              .component_count = 3,
              .luma_sampling   = 0x11,
              .entropy         = entropy,
              .entropy_size    = sizeof entropy },
            "unsupported-jpeg-frame" },

        /* An SOF9 frame. */
        { { .frame_marker      = 0xc9,
              .precision       = 8,
              .width           = 16,
              .height          = 8,
              .component_count = 3,
              .luma_sampling   = 0x11,
              .entropy         = entropy,
              .entropy_size    = sizeof entropy },
            "unsupported-jpeg-frame" },

        /* 12-bit samples. */
        { { .frame_marker      = 0xc0,
              .precision       = 12,
              .width           = 16,
              .height          = 8,
              .component_count = 3,
              .luma_sampling   = 0x11,
              .entropy         = entropy,
              .entropy_size    = sizeof entropy },
            "unsupported-jpeg-precision" },

        /* Luma sampled 0x41. */
        { { .frame_marker      = 0xc0,
              .precision       = 8,
              .width           = 16,
              .height          = 8,
              .component_count = 3,
              .luma_sampling   = 0x41,
              .entropy         = entropy,
              .entropy_size    = sizeof entropy },
            "unsupported-jpeg-sampling" },

        /* One component, sampled 0x22. */
        { { .frame_marker      = 0xc0,
              .precision       = 8,
              .width           = 8,
              .height          = 8,
              .component_count = 1,
              .luma_sampling   = 0x22,
              .entropy         = entropy,
              .entropy_size    = sizeof entropy },
            "unsupported-jpeg-sampling" },

        /* The EXIF orientation 6. */
        { { .frame_marker      = 0xc0,
              .precision       = 8,
              .width           = 16,
              .height          = 8,
              .component_count = 3,
              .luma_sampling   = 0x11,
              .exif            = exif_rotated,
              .exif_size       = sizeof exif_rotated,
              .entropy         = entropy,
              .entropy_size    = sizeof entropy },
            "unsupported-jpeg-orientation" }
    };

    for (size_t at = 0; at < sizeof cases / sizeof cases[0]; ++at) {
        uint8_t      file[512];
        const size_t size = fixture_jpeg_build(file, sizeof file, &cases[at].spec);
        edds_error   error;

        memset(&error, 0, sizeof error);
        CHECK(size != 0);
        CHECK(refused_by(file, size, EDDS_SOURCE_JPG, &error) == EDDS_UNSUPPORTED_FORMAT);
        CHECK(strcmp(error.code, cases[at].code) == 0);
    }

    return 1;
}

/**
 * A 16x8 JPEG whose entropy-coded data stops partway through the scan and which has no
 * end-of-image marker: refused as invalid, with "truncated-jpeg-scan". Every prefix of the same
 * JPEG built with its end-of-image marker is refused as well.
 */
static int damaged_jpeg_input_fails_without_partial_output(void) {
    /* Entropy-coded data that stops partway through the scan. */
    static const uint8_t entropy[] = { 0x8d, 0xe0, 0xf2 };

    /* The JPEG without its end-of-image marker. */
    const fixture_jpeg_spec truncated = {
        .frame_marker      = 0xc0,
        .precision         = 8,
        .width             = 16,
        .height            = 8,
        .component_count   = 3,
        .luma_sampling     = 0x11,
        .entropy           = entropy,
        .entropy_size      = sizeof entropy,
        .omit_end_of_image = 1
    };

    /* The same JPEG with it. */
    const fixture_jpeg_spec complete = {
        .frame_marker    = 0xc0,
        .precision       = 8,
        .width           = 16,
        .height          = 8,
        .component_count = 3,
        .luma_sampling   = 0x11,
        .entropy         = entropy,
        .entropy_size    = sizeof entropy
    };

    /* A JPEG file and its size, and the error a refusal fills in. */
    uint8_t    file[512];
    size_t     size = fixture_jpeg_build(file, sizeof file, &truncated);
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

/** The size of the TIFF tests' 3x2 RGB samples, one byte each. */
enum {
    TIFF_PIXEL_BYTES = 18
};

/** The TIFF tests' image: two rows of three RGB pixels. */
static const uint8_t tiff_rgb_pixels[TIFF_PIXEL_BYTES] = {
    10, 20, 30, 40, 50, 60, 70, 80, 90,
    100, 110, 120, 130, 140, 150, 160, 170, 180
};

/**
 * A legal, if wasteful, LZW stream: clear, every byte as its own code, end.
 * The codes are 9 bits each, written highest bit first. Returns the stream's size in bytes, or 0
 * when it does not fit the capacity.
 */
static size_t pack_lzw_literals(uint8_t *output, size_t capacity, const uint8_t *data, size_t size) {
    const size_t codes = size + 2u;
    const size_t bytes = (codes * 9u + 7u) / 8u;
    size_t       bit   = 0;

    if (capacity < bytes) {
        return 0;
    }

    memset(output, 0, bytes);

    for (size_t at = 0; at < codes; ++at) {
        /* The clear code, 256, first; the end code, 257, last; the data's bytes between. */
        const uint32_t code = at == 0u ? 256u : at == codes - 1u ? 257u
                                                                 : data[at - 1u];

        /* The code's nine bits, the highest first, set from the next free bit of the output. */
        for (uint32_t step = 0; step < 9u; ++step) {
            if (((code >> (8u - step)) & 1u) != 0u) {
                output[(bit + step) / 8u] |= (uint8_t)(1u << (7u - (bit + step) % 8u));
            }
        }

        bit += 9u;
    }

    return bytes;
}

/**
 * A zlib stream that holds the data in one stored block, followed by its Adler-32. Returns its
 * size, 11 bytes more than the data, or 0 when it does not fit the capacity or the data is over
 * 0xffff bytes.
 */
static size_t pack_zlib_stored(uint8_t *output, size_t capacity, const uint8_t *data, size_t size) {
    uint32_t first  = 1;
    uint32_t second = 0;

    if (capacity < size + 11u || size > 0xffffu) {
        return 0;
    }

    /* The zlib header, the block's header, its length and the length's complement, the data. */
    output[0] = 0x78;
    output[1] = 0x01;
    output[2] = 0x01;
    output[3] = (uint8_t)size;
    output[4] = (uint8_t)(size >> 8);
    output[5] = (uint8_t)~(uint8_t)size;
    output[6] = (uint8_t)~(uint8_t)(size >> 8);
    memcpy(output + 7, data, size);

    /* The Adler-32 of the data, most significant byte first. */
    for (size_t at = 0; at < size; ++at) {
        first  = (first + data[at]) % 65521u;
        second = (second + first) % 65521u;
    }

    output[7 + size]  = (uint8_t)(second >> 8);
    output[8 + size]  = (uint8_t)second;
    output[9 + size]  = (uint8_t)(first >> 8);
    output[10 + size] = (uint8_t)first;

    return size + 11u;
}

/**
 * The nine-tag baseline RGB directory every compression case here shares.
 * Builds a 3x2 RGB TIFF in either byte order around one strip and returns its size, or 0 when the
 * strip is over 256 bytes or the file does not fit the capacity.
 */
static size_t build_rgb_tiff(
    uint8_t       *output,
    size_t         capacity,
    int            big_endian,
    uint32_t       compression,
    const uint8_t *strip,
    size_t         strip_size) {
    /* After the directory: the bits per sample, then, 6 bytes on, the strip. */
    const uint32_t bits_at   = (uint32_t)fixture_tiff_ifd_end(9);
    const uint32_t pixels_at = bits_at + 6u;

    /*
     * Each tag with its type (3 for 16 bits, 4 for 32), count and value: width 3, height 2, the
     * bits per sample (three values at bits_at), the compression, photometric 2 (RGB), the strip's
     * offset, 3 samples per pixel, 2 rows per strip, and the strip's byte count.
     */
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

    /* Three 16-bit sample sizes of 8, in the file's byte order, then the strip. */
    memset(trailing, 0, sizeof trailing);
    trailing[big_endian ? 1 : 0] = 8;
    trailing[big_endian ? 3 : 2] = 8;
    trailing[big_endian ? 5 : 4] = 8;
    memcpy(trailing + 6, strip, strip_size);

    return fixture_tiff_build(output, capacity, big_endian, tags, 9, trailing, 6u + strip_size);
}

/**
 * The 3x2 RGB image stored six ways: uncompressed in little- and big-endian byte order, then as
 * PackBits, LZW, Deflate (8) and old-style Deflate (32946). Converted without mips, every one
 * comes out 3x2 BGRX with the same pixels.
 */
static int tiff_compressions_and_byte_orders_agree_on_one_image(void) {
    /* The RGBA every case must decode to. */
    static const uint8_t expected[] = {
        10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255,
        100, 110, 120, 255, 130, 140, 150, 255, 160, 170, 180, 255
    };

    /* The samples packed as PackBits, LZW and zlib, and the LZW stream's size. */
    uint8_t      packbits[TIFF_PIXEL_BYTES + 1];
    uint8_t      lzw[64];
    uint8_t      deflate[TIFF_PIXEL_BYTES + 16];
    const size_t lzw_size = pack_lzw_literals(lzw, sizeof lzw, tiff_rgb_pixels, TIFF_PIXEL_BYTES);

    /* The zlib stream's size. */
    const size_t deflate_size = pack_zlib_stored(deflate, sizeof deflate, tiff_rgb_pixels, TIFF_PIXEL_BYTES);

    /*
     * Each case: the compression, the strip and its size (set below for LZW and Deflate), and
     * whether the file is big-endian.
     */
    struct {
        uint32_t       compression;
        const uint8_t *strip;
        size_t         size;
        int            big_endian;
    } cases[] = {
        { 1, tiff_rgb_pixels, TIFF_PIXEL_BYTES, 0 },
        { 1, tiff_rgb_pixels, TIFF_PIXEL_BYTES, 1 },
        { 32773, packbits, sizeof packbits, 0 },
        { 5, lzw, 0, 0 },
        { 8, deflate, 0, 0 },
        { 32946, deflate, 0, 0 }
    };

    /* PackBits: a control byte of 17, which copies the next 18 bytes as they are. */
    packbits[0] = (uint8_t)(TIFF_PIXEL_BYTES - 1u);
    memcpy(packbits + 1, tiff_rgb_pixels, TIFF_PIXEL_BYTES);

    /* The LZW and zlib streams were built; their sizes go into their cases. */
    CHECK(lzw_size != 0 && deflate_size != 0);
    cases[3].size = lzw_size;
    cases[4].size = deflate_size;
    cases[5].size = deflate_size;

    for (size_t at = 0; at < sizeof cases / sizeof cases[0]; ++at) {
        uint8_t   file[512];
        edds_info info;
        uint8_t  *rgba      = NULL;
        size_t    rgba_size = 0;

        /* The TIFF for this case, and its size: 0 if it could not be built. */
        const size_t size = build_rgb_tiff(file, sizeof file, cases[at].big_endian, cases[at].compression, cases[at].strip, cases[at].size);

        CHECK(size != 0);
        CHECK(decoded_through_convert(file, size, EDDS_SOURCE_TIFF, 0, &info, &rgba, &rgba_size));
        CHECK(info.width == 3 && info.height == 2 && info.pixel_format == EDDS_PIXEL_BGRX8);
        CHECK(rgba_size == sizeof expected && memcmp(rgba, expected, sizeof expected) == 0);
        edds_free(rgba);
    }

    return 1;
}

/**
 * Alpha is a fact of the source: it arrives only where an unassociated extra sample declares it.
 * A 2x1 TIFF of four samples, the fourth an extra sample of kind 2, converts to BGRA with its
 * pixels unchanged; with the extra sample's kind set to 1 it is refused, as
 * "unsupported-tiff-alpha".
 */
static int tiff_unassociated_extra_sample_is_the_only_alpha(void) {
    /* Two pixels of four samples. */
    static const uint8_t pixels[] = { 10, 20, 30, 40, 50, 60, 70, 80 };

    /* After the directory: the bits per sample, then, 8 bytes on, the pixels. */
    const uint32_t bits_at   = (uint32_t)fixture_tiff_ifd_end(10);
    const uint32_t pixels_at = bits_at + 8u;

    /*
     * The tags: width 2, height 1, four bits-per-sample values, no compression, photometric 2
     * (RGB), the strip's offset, 4 samples per pixel, 1 row per strip, the strip's byte count, and
     * one extra sample (338) of kind 2, unassociated alpha.
     */
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

    /* The bytes after the directory, and the file. */
    uint8_t trailing[8 + sizeof pixels];
    uint8_t file[512];

    /* The output's inspection and its top level as RGBA, a refusal's error, the file's size. */
    edds_info  info;
    uint8_t   *rgba      = NULL;
    size_t     rgba_size = 0;
    edds_error error;
    size_t     size;

    /* Four 16-bit sample sizes of 8, little-endian, then the pixels. */
    memset(trailing, 0, sizeof trailing);
    trailing[0] = 8;
    trailing[2] = 8;
    trailing[4] = 8;
    trailing[6] = 8;
    memcpy(trailing + 8, pixels, sizeof pixels);

    /* Unassociated alpha: BGRA, the samples as they are. */
    size = fixture_tiff_build(file, sizeof file, 0, tags, 10, trailing, sizeof trailing);
    CHECK(size != 0);
    CHECK(decoded_through_convert(file, size, EDDS_SOURCE_TIFF, 0, &info, &rgba, &rgba_size));
    CHECK(info.pixel_format == EDDS_PIXEL_BGRA8);
    CHECK(rgba_size == sizeof pixels && memcmp(rgba, pixels, sizeof pixels) == 0);
    edds_free(rgba);

    /* Premultiplied alpha is a different sample meaning, so it is refused rather than shown. */
    tags[9].value = 1;
    size          = fixture_tiff_build(file, sizeof file, 0, tags, 10, trailing, sizeof trailing);
    memset(&error, 0, sizeof error);
    CHECK(size != 0);
    CHECK(refused_by(file, size, EDDS_SOURCE_TIFF, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-tiff-alpha") == 0);

    return 1;
}

/**
 * The 3x2 RGB TIFF with one tag changed or added at a time: each is refused as unsupported, with
 * the error code its case names.
 */
static int unsupported_tiff_subtypes_are_refused_before_any_pixel(void) {
    /* Each change: the tag, its value, and the error code it must be refused with. */
    const struct {
        uint16_t    tag;
        uint32_t    value;
        const char *code;
    } changes[] = {
        { 262, 3, "unsupported-tiff-channels" },     /* Photometric 3. */
        { 284, 2, "unsupported-tiff-layout" },       /* Planar configuration 2. */
        { 317, 2, "unsupported-tiff-predictor" },    /* Predictor 2. */
        { 274, 6, "unsupported-tiff-orientation" },  /* Orientation 6. */
        { 322, 8, "unsupported-tiff-layout" },       /* A tile width of 8. */
        { 266, 2, "unsupported-tiff-layout" },       /* Fill order 2. */
        { 259, 7, "unsupported-tiff-compression" },  /* Compression 7. */
        { 339, 3, "unsupported-tiff-sample-format" } /* Sample format 3. */
    };

    for (size_t at = 0; at < sizeof changes / sizeof changes[0]; ++at) {
        /* After the directory: the bits per sample, then, 6 bytes on, the pixels. */
        const uint32_t bits_at   = (uint32_t)fixture_tiff_ifd_end(10);
        const uint32_t pixels_at = bits_at + 6u;

        /* The nine tags of the baseline RGB image, then the change as a tenth. */
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

        /* The bytes after the directory, the file and its size, and a refusal's error. */
        uint8_t    trailing[6 + TIFF_PIXEL_BYTES];
        uint8_t    file[512];
        edds_error error;
        size_t     size;

        for (size_t entry = 0; entry < 9u; ++entry) {
            if (tags[entry].tag == changes[at].tag) {
                /* A tag the baseline already carries is overridden where it stands. */
                tags[entry].value = changes[at].value;
                tags[9]           = tags[8];
            }
        }

        /* Three 16-bit sample sizes of 8, little-endian, then the pixels. */
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

/**
 * The 3x2 RGB TIFF with 16 bits per sample and a strip of twice the bytes: refused as
 * unsupported, with "unsupported-tiff-bit-depth".
 */
static int sixteen_bit_tiff_samples_are_refused(void) {
    /* After the directory: the bits per sample, then, 6 bytes on, the pixels. */
    const uint32_t bits_at   = (uint32_t)fixture_tiff_ifd_end(9);
    const uint32_t pixels_at = bits_at + 6u;

    /* The nine tags of the baseline RGB image, with a strip of twice the bytes. */
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

    /* The bytes after the directory, the file and its size, and a refusal's error. */
    uint8_t    trailing[6 + TIFF_PIXEL_BYTES * 2];
    uint8_t    file[512];
    edds_error error;
    size_t     size;

    /* Three sample sizes of 16, then a strip of zeros. */
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

/**
 * The 3x2 RGB TIFF with its next-directory pointer set to 128, as if a second page followed:
 * refused as unsupported, with "unsupported-tiff-pages".
 */
static int a_second_tiff_page_is_refused_rather_than_silently_dropped(void) {
    uint8_t      file[512];
    edds_error   error;
    const size_t size = build_rgb_tiff(file, sizeof file, 0, 1, tiff_rgb_pixels, TIFF_PIXEL_BYTES);

    CHECK(size != 0);

    /* The next-directory pointer sits immediately after the nine entries. */
    file[8u + 2u + 9u * 12u] = 0x80;
    memset(&error, 0, sizeof error);
    CHECK(refused_by(file, size, EDDS_SOURCE_TIFF, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strcmp(error.code, "unsupported-tiff-pages") == 0);

    return 1;
}

/** Every prefix of the 3x2 uncompressed RGB TIFF shorter than the whole file is refused. */
static int damaged_tiff_input_fails_without_partial_output(void) {
    uint8_t      file[512];
    const size_t size = build_rgb_tiff(file, sizeof file, 0, 1, tiff_rgb_pixels, TIFF_PIXEL_BYTES);

    CHECK(size != 0);

    for (size_t prefix = 0; prefix < size; ++prefix) {
        edds_error error;

        memset(&error, 0, sizeof error);
        CHECK(refused_by(file, prefix, EDDS_SOURCE_TIFF, &error) != EDDS_OK);
    }

    return 1;
}

/**
 * One table every part of the converter reads, rather than four remembered lists of its own.
 * Each of the five source capabilities is found again by its format and by its resource class;
 * an alias, NULL and an unknown format find none.
 */
static int the_source_contract_names_only_registered_resource_classes(void) {
    size_t                        count        = 0;
    const edds_source_capability *capabilities = edds_source_capabilities(&count);

    CHECK(capabilities != NULL && count == 5u);

    for (size_t at = 0; at < count; ++at) {
        CHECK(edds_source_capability_of_format(capabilities[at].format) == &capabilities[at]);
        CHECK(edds_source_capability_of_resource_class(capabilities[at].resource_class) == &capabilities[at]);
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

/** The gradient source's size: 9x5 pixels. */
enum {
    GRADIENT_WIDTH  = 9,
    GRADIENT_HEIGHT = 5,
    GRADIENT_PIXELS = GRADIENT_WIDTH * GRADIENT_HEIGHT
};

/** The alpha a gradient source carries: no channel, an opaque one, one sample of 254, or a ramp. */
typedef enum source_alpha_shape {
    SOURCE_NO_ALPHA_CHANNEL,
    SOURCE_FULLY_OPAQUE_ALPHA,
    SOURCE_ONE_SAMPLE_BELOW_OPAQUE,
    SOURCE_ALPHA_RAMP
} source_alpha_shape;

/**
 * A colour ramp wide enough for three block columns.
 * Fills the 9x5 BGRA samples: red rises from left to right while blue falls, green rises from top
 * to bottom, and alpha follows the shape asked for.
 */
static void gradient_bgra(uint8_t bgra[GRADIENT_PIXELS * 4], source_alpha_shape alpha) {
    for (uint32_t y = 0; y < GRADIENT_HEIGHT; ++y) {
        for (uint32_t x = 0; x < GRADIENT_WIDTH; ++x) {
            uint8_t      *pixel = bgra + ((size_t)y * GRADIENT_WIDTH + x) * 4u;
            const uint8_t red   = (uint8_t)((x * 255u) / (GRADIENT_WIDTH - 1u));
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

/** The smooth and quadrant sources' size: 16x16 pixels. */
enum {
    SMOOTH_SIDE   = 16,
    SMOOTH_PIXELS = SMOOTH_SIDE * SMOOTH_SIDE
};

/**
 * A gentle blend, which is what a texture usually is. The error bounds are claimed against this
 * rather than against the ramp above: a ramp that moves a quarter of the range inside one block
 * measures how a single line through colour space fails, not how well the encoder finds it.
 * Fills the 16x16 BGRA samples: blue falls and red rises across, green rises and alpha falls down.
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

/**
 * Four flat quadrants, so the same block repeats and the container has something to compress.
 * Fills the 16x16 BGRA samples with one colour and alpha for each 8x8 quadrant.
 */
static void quadrant_bgra(uint8_t bgra[SMOOTH_PIXELS * 4]) {
    for (uint32_t y = 0; y < SMOOTH_SIDE; ++y) {
        for (uint32_t x = 0; x < SMOOTH_SIDE; ++x) {
            uint8_t       *pixel    = bgra + ((size_t)y * SMOOTH_SIDE + x) * 4u;
            const unsigned quadrant = (x < SMOOTH_SIDE / 2u ? 0u : 1u) + (y < SMOOTH_SIDE / 2u ? 0u : 2u);

            pixel[0] = (uint8_t)(30u + quadrant * 60u);
            pixel[1] = (uint8_t)(200u - quadrant * 50u);
            pixel[2] = (uint8_t)(80u + quadrant * 40u);
            pixel[3] = (uint8_t)(255u - quadrant * 30u);
        }
    }
}

/**
 * Converts a synthetic TGA and hands back the inspection plus one decoded mip.
 * The TGA, up to 64x64, is built from the BGRA samples with or without alpha and converted under
 * the profile; the level asked for is previewed unless rgba is NULL. Returns 1 when every step
 * succeeds, with *rgba for the caller to free with edds_free, and 0 otherwise.
 */
static int converted_source(
    const uint8_t      *bgra,
    uint32_t            width,
    uint32_t            height,
    int                 with_alpha,
    const edds_profile *profile,
    uint32_t            level,
    edds_info          *info,
    uint8_t           **rgba,
    size_t             *rgba_size) {
    uint8_t      tga[18u + 64u * 64u * 4u];
    const size_t size   = fixture_tga_build(tga, sizeof tga, width, height, with_alpha, bgra);
    FILE        *source = size == 0 ? NULL : stream_of(tga, size);
    FILE        *output = temporary();
    edds_error   error;
    int          ok;

    if (source == NULL || output == NULL) {
        if (source != NULL) {
            fclose(source);
        }

        if (output != NULL) {
            fclose(output);
        }

        return 0;
    }

    /* Converted, inspected and previewed, each step only if the one before succeeded. */
    ok = edds_convert(source, EDDS_SOURCE_TGA, output, profile,
             never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK &&
        fseek(output, 0, SEEK_SET) == 0 &&
        edds_inspect(output, info, never_cancelled, NULL, &error) == EDDS_OK &&
        (rgba == NULL ||
            edds_preview(output, info, level, never_cancelled, NULL,
                rgba, rgba_size, &error) == EDDS_OK);

    fclose(source);
    fclose(output);

    return ok;
}

/**
 * Mips of sources whose sides are not powers of two. A 5x3 source whose red is y * 10 + x gives a
 * 2x1 second level of red 10 and 13 through the box filter; black with one red pixel at (2, 1),
 * it gives a black one through the Kaiser filter. A 5x1 and a 1x5 line of red 0 to 40 both give
 * red 8 and 32 through the box filter.
 */
static int box_and_kaiser_mips_cover_npot_edges_exactly(void) {
    /* The second level each filter must give for the 5x3 sources. */
    static const uint8_t expected_box[]    = { 10, 0, 0, 255, 13, 0, 0, 255 };
    static const uint8_t expected_kaiser[] = { 0, 0, 0, 255, 0, 0, 0, 255 };

    /* The 5x3 source's BGRA samples. */
    uint8_t bgra[5u * 3u * 4u] = { 0 };

    /* The conversion settings, the inspected output, and its previewed level. */
    edds_profile profile;
    edds_info    info;
    uint8_t     *rgba      = NULL;
    size_t       rgba_size = 0;

    /* Red y * 10 + x, every pixel opaque. */
    for (uint32_t y = 0; y < 3u; ++y) {
        for (uint32_t x = 0; x < 5u; ++x) {
            const size_t at = ((size_t)y * 5u + x) * 4u;

            bgra[at + 2u] = (uint8_t)(y * 10u + x);
            bgra[at + 3u] = 255u;
        }
    }

    /* The box filter: three levels, and the second as expected. */
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    CHECK(converted_source(bgra, 5, 3, 1, &profile, 1, &info, &rgba, &rgba_size));
    CHECK(info.mip_count == 3u && rgba_size == sizeof expected_box);
    CHECK(memcmp(rgba, expected_box, sizeof expected_box) == 0);
    edds_free(rgba);
    rgba = NULL;

    /* Black and opaque, but for one red pixel at (2, 1). */
    memset(bgra, 0, sizeof bgra);

    for (size_t at = 0; at < 15u; ++at) {
        bgra[at * 4u + 3u] = 255u;
    }

    bgra[(1u * 5u + 2u) * 4u + 2u] = 255u;

    /* The Kaiser filter: the second level black. */
    profile.mipmap_filter = EDDS_FILTER_KAISER;
    CHECK(converted_source(bgra, 5, 3, 1, &profile, 1, &info, &rgba, &rgba_size));
    CHECK(rgba_size == sizeof expected_kaiser);
    CHECK(memcmp(rgba, expected_kaiser, sizeof expected_kaiser) == 0);
    edds_free(rgba);

    {
        /* The second level both lines must give. */
        static const uint8_t expected_line[] = { 8, 0, 0, 255, 32, 0, 0, 255 };

        /* A 5x1 and a 1x5 line of the same samples. */
        uint8_t horizontal[5u * 4u] = { 0 };
        uint8_t vertical[5u * 4u]   = { 0 };

        profile.mipmap_filter = EDDS_FILTER_BOX;

        /* Red 0, 10, 20, 30, 40, every pixel opaque. */
        for (size_t at = 0; at < 5u; ++at) {
            horizontal[at * 4u + 2u] = (uint8_t)(at * 10u);
            horizontal[at * 4u + 3u] = 255u;
            vertical[at * 4u + 2u]   = (uint8_t)(at * 10u);
            vertical[at * 4u + 3u]   = 255u;
        }

        /* The 5x1 line: a 2x1 second level. */
        rgba = NULL;
        CHECK(converted_source(horizontal, 5, 1, 1, &profile, 1, &info, &rgba, &rgba_size));
        CHECK(info.mips[1].width == 2u && info.mips[1].height == 1u);
        CHECK(rgba_size == sizeof expected_line && memcmp(rgba, expected_line, sizeof expected_line) == 0);
        edds_free(rgba);
        rgba = NULL;

        /* The 1x5 line: a 1x2 second level of the same two pixels. */
        CHECK(converted_source(vertical, 1, 5, 1, &profile, 1, &info, &rgba, &rgba_size));
        CHECK(info.mips[1].width == 1u && info.mips[1].height == 2u);
        CHECK(rgba_size == sizeof expected_line && memcmp(rgba, expected_line, sizeof expected_line) == 0);
        edds_free(rgba);
    }

    return 1;
}

/**
 * DDS sources of every runtime format, converted without mips and checked through their first
 * pixel: top-only DXT1 and DXT5 files, then 4x4 DX10 files of nine formats, each payload one
 * sample or block repeated. Last, an R8 DX10 file with three supplied levels, converted with the
 * Red conversion, keeps all three.
 */
static int dds_source_matrix_covers_legacy_blocks_and_dx10_supplied_levels(void) {
    /* The top-only DXT1 and DXT5 sources, and the R8 one with three levels. */
    test_bytes dxt1 = fixture_dds_dxt1_top();
    test_bytes dxt5 = fixture_dds_dxt5_top();
    test_bytes r8   = fixture_dds_dx10_r8_mips();

    /* The conversion settings, the inspected output, and the error a failed call fills in. */
    edds_profile profile;
    edds_info    info;
    edds_error   error;

    /* A previewed level. */
    uint8_t *rgba      = NULL;
    size_t   rgba_size = 0;

    /* The R8 source and its conversion, as files. */
    FILE *source;
    FILE *output;

    CHECK(dxt1.data != NULL && dxt5.data != NULL && r8.data != NULL);

    /* DXT1 of zeros: 4x4 in one level, every pixel black and opaque. */
    CHECK(decoded_through_convert(dxt1.data, dxt1.size, EDDS_SOURCE_DDS, 0, &info, &rgba, &rgba_size));
    CHECK(info.width == 4u && info.height == 4u && info.mip_count == 1u);
    CHECK(rgba_size == 4u * 4u * 4u && every_pixel_is(rgba, rgba_size, 0, 0, 0));
    edds_free(rgba);
    rgba = NULL;

    /* DXT5 of zeros: the first pixel black, with alpha 0. */
    CHECK(decoded_through_convert(dxt5.data, dxt5.size, EDDS_SOURCE_DDS, 0, &info, &rgba, &rgba_size));
    CHECK(info.width == 4u && info.height == 4u && info.mip_count == 1u);
    CHECK(rgba_size == 4u * 4u * 4u && rgba[0] == 0u && rgba[1] == 0u && rgba[2] == 0u && rgba[3] == 0u);
    edds_free(rgba);
    rgba = NULL;

    {
        /* The samples and blocks the DX10 payloads are made of. */
        static const uint8_t r[]        = { 17 };
        static const uint8_t rg[]       = { 17, 33 };
        static const uint8_t zero8[8]   = { 0 };
        static const uint8_t zero16[16] = { 0 };
        static const uint8_t bgra[]     = { 3, 2, 1, 4 };
        static const uint8_t bc7[]      = {
            0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x81, 0x40,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };

        /*
         * Each case: the DXGI format (R8, R8G8, BC1, BC3, BC4, BC5, B8G8R8A8, B8G8R8X8, BC7), the
         * sample or block and its size, the bytes per pixel (0 for a block format, whose 4x4
         * payload is one block), and the first pixel's red, green, blue and alpha.
         */
        static const struct {
            uint32_t       dxgi;
            const uint8_t *sample;
            size_t         sample_size;
            uint32_t       bytes_per_pixel;
            uint8_t        red;
            uint8_t        green;
            uint8_t        blue;
            uint8_t        alpha;
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
            const size_t pixels       = cases[at].bytes_per_pixel == 0u ? 1u : 16u;
            const size_t payload_size = cases[at].sample_size * pixels;
            uint8_t      payload[64]  = { 0 };
            test_bytes   fixture;

            /* The payload: the sample for each of the 16 pixels, or the one block. */
            for (size_t pixel = 0; pixel < pixels; ++pixel) {
                memcpy(payload + pixel * cases[at].sample_size, cases[at].sample, cases[at].sample_size);
            }

            /* The 4x4 DX10 file around it, converted without mips: its first pixel as expected. */
            fixture = fixture_dds_dx10_top(cases[at].dxgi, payload, payload_size, cases[at].bytes_per_pixel);
            CHECK(fixture.data != NULL);
            CHECK(decoded_through_convert(fixture.data, fixture.size, EDDS_SOURCE_DDS, 0, &info, &rgba, &rgba_size));
            CHECK(info.width == 4u && info.height == 4u && rgba_size == 4u * 4u * 4u);
            CHECK(rgba[0] == cases[at].red && rgba[1] == cases[at].green && rgba[2] == cases[at].blue && rgba[3] == cases[at].alpha);
            edds_free(rgba);
            rgba = NULL;
            fixture_free(fixture);
        }
    }

    /* The R8 file with its three supplied levels, converted with the Red conversion as COPY. */
    source = stream_of(r8.data, r8.size);
    output = temporary();
    CHECK(source != NULL && output != NULL);
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    profile.conversion      = EDDS_CONVERSION_RED;
    profile.contains_mips   = 1;
    profile.generate_mips   = 0;
    CHECK(edds_convert(source, EDDS_SOURCE_DDS, output, &profile, never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);

    /* R8, 4x2 in three levels. */
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.width == 4u && info.height == 2u && info.mip_count == 3u && info.pixel_format == EDDS_PIXEL_R8);

    /* Each level's first pixel: the first red value its source level holds, opaque. */
    for (uint32_t level = 0; level < info.mip_count; ++level) {
        static const uint8_t expected[] = { 1, 11, 33 };

        CHECK(edds_preview(output, &info, level, never_cancelled, NULL, &rgba, &rgba_size, &error) == EDDS_OK);
        CHECK(rgba_size == (size_t)info.mips[level].width * info.mips[level].height * 4u);
        CHECK(rgba[0] == expected[level] && rgba[1] == 0u && rgba[2] == 0u && rgba[3] == 255u);
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

/**
 * The 4x2 BGRA DDS whose top level is opaque and whose two smaller levels have alpha 128 and 64,
 * converted with the DXT conversion and its own mips: the output is DXT5, and its last level
 * keeps alpha below 255.
 */
static int dds_alpha_branch_scans_every_supplied_level(void) {
    /* The DDS source, and the converted file. */
    test_bytes dds    = fixture_dds_bgra_alpha_mips();
    FILE      *source = stream_of(dds.data, dds.size);
    FILE      *output = temporary();

    /* The conversion settings, the inspected output, and the error a failed call fills in. */
    edds_profile profile;
    edds_info    info;
    edds_error   error;

    /* The previewed level. */
    uint8_t *rgba      = NULL;
    size_t   rgba_size = 0;

    CHECK(dds.data != NULL && source != NULL && output != NULL);

    /* The DXT conversion of the levels the DDS supplies, stored as COPY. */
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    profile.conversion      = EDDS_CONVERSION_DXT;
    profile.contains_mips   = 1;
    profile.generate_mips   = 0;
    CHECK(edds_convert(source, EDDS_SOURCE_DDS, output, &profile, never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);

    /* DXT5 in three levels, the 1x1 last one with its alpha. */
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.pixel_format == EDDS_PIXEL_DXT5 && info.mip_count == 3u);
    CHECK(edds_preview(output, &info, 2, never_cancelled, NULL, &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == 4u && rgba[3] < 255u);

    edds_free(rgba);
    fclose(source);
    fclose(output);
    fixture_free(dds);

    return 1;
}

/**
 * A 2x1 source read as a normal map, converted as COPY. With the Normalize flag, level 0 is the
 * source normalized and level 1 the average of that; with the Normalize mip function instead,
 * level 1 is the average, normalized. Alpha is averaged either way.
 */
static int normalize_flag_and_mipmap_function_have_distinct_stages(void) {
    /* The source, as BGRA. */
    static const uint8_t source_bgra[] = {
        127, 127, 191, 33,
        127, 191, 127, 77
    };

    /* What the Normalize flag must give, as RGBA: level 0, then level 1. */
    static const uint8_t expected_normalized_source[] = {
        255, 127, 127, 33,
        127, 255, 127, 77
    };
    static const uint8_t expected_pre_normalized_mip[] = { 191, 191, 127, 55 };

    /* What the Normalize mip function must give for level 1. */
    static const uint8_t expected_post_normalized_mip[] = { 218, 218, 126, 55 };

    /* The conversion settings, the inspected output, and its previewed level. */
    edds_profile profile;
    edds_info    info;
    uint8_t     *rgba      = NULL;
    size_t       rgba_size = 0;

    /* The Normalize flag: levels 0 and 1. */
    edds_default_profile(&profile);
    profile.format_compress = EDDS_COMPRESS_COPY;
    profile.normalize       = 1;
    CHECK(converted_source(source_bgra, 2, 1, 1, &profile, 0, &info, &rgba, &rgba_size));
    CHECK(rgba_size == sizeof expected_normalized_source &&
        memcmp(rgba, expected_normalized_source, sizeof expected_normalized_source) == 0);
    edds_free(rgba);
    rgba = NULL;

    CHECK(converted_source(source_bgra, 2, 1, 1, &profile, 1, &info, &rgba, &rgba_size));
    CHECK(rgba_size == sizeof expected_pre_normalized_mip &&
        memcmp(rgba, expected_pre_normalized_mip, sizeof expected_pre_normalized_mip) == 0);
    edds_free(rgba);
    rgba = NULL;

    /* The Normalize mip function instead: level 1. */
    profile.normalize       = 0;
    profile.mipmap_function = EDDS_MIPMAP_NORMALIZE;
    CHECK(converted_source(source_bgra, 2, 1, 1, &profile, 1, &info, &rgba, &rgba_size));
    CHECK(rgba_size == sizeof expected_post_normalized_mip &&
        memcmp(rgba, expected_post_normalized_mip, sizeof expected_post_normalized_mip) == 0);
    edds_free(rgba);

    return 1;
}

/**
 * Converts the 9x5 gradient, with its alpha ramp, under the profile and returns the conversion's
 * status, or EDDS_OK (which no refusal is) when a temporary file cannot be made.
 */
static edds_status refused_profile(const edds_profile *profile, edds_error *error) {
    uint8_t     bgra[GRADIENT_PIXELS * 4];
    uint8_t     tga[18u + GRADIENT_PIXELS * 4u];
    FILE       *source;
    FILE       *output = temporary();
    edds_status status;

    /* The gradient as a TGA, in a temporary file. */
    gradient_bgra(bgra, SOURCE_ALPHA_RAMP);
    source = stream_of(tga, fixture_tga_build(tga, sizeof tga, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, bgra));

    if (source == NULL || output == NULL) {
        if (source != NULL) {
            fclose(source);
        }

        if (output != NULL) {
            fclose(output);
        }

        return EDDS_OK;
    }

    status = edds_convert(source, EDDS_SOURCE_TGA, output, profile, never_cancelled, NULL, NULL, NULL, error);

    fclose(source);
    fclose(output);

    return status;
}

/** Blocks a mip of this size needs, which is the rule the padding exists to satisfy. */
static uint32_t block_count_of(uint32_t width, uint32_t height) {
    return ((width + 3u) / 4u) * ((height + 3u) / 4u);
}

/**
 * The mean absolute difference between two RGBA images, taken over the listed channels (0 for red
 * to 3 for alpha) of every pixel.
 */
static double mean_channel_error(
    const uint8_t  *decoded,
    const uint8_t  *expected,
    size_t          pixels,
    const unsigned *channels,
    unsigned        channel_count) {
    double total = 0;

    for (size_t pixel = 0; pixel < pixels; ++pixel) {
        for (unsigned at = 0; at < channel_count; ++at) {
            const int difference = (int)decoded[pixel * 4u + channels[at]] - (int)expected[pixel * 4u + channels[at]];

            total += difference < 0 ? -difference : difference;
        }
    }

    return total / (double)(pixels * channel_count);
}

/**
 * The gradient as straight RGBA, which is what a decode of a lossless result must equal.
 * Copies the BGRA samples into rgba with blue and red swapped.
 */
static void gradient_rgba(const uint8_t *bgra, uint8_t *rgba, size_t pixels) {
    for (size_t pixel = 0; pixel < pixels; ++pixel) {
        rgba[pixel * 4u]      = bgra[pixel * 4u + 2u];
        rgba[pixel * 4u + 1u] = bgra[pixel * 4u + 1u];
        rgba[pixel * 4u + 2u] = bgra[pixel * 4u];
        rgba[pixel * 4u + 3u] = bgra[pixel * 4u + 3u];
    }
}

/**
 * The 9x5 gradient with an alpha ramp, converted under each conversion as COPY: each output
 * declares its runtime format, FourCC, DXGI format and channels, can be previewed, holds four
 * levels of the expected decoded size, and gives its top level's pitch or linear size.
 */
static int every_conversion_stores_its_proven_runtime_format(void) {
    /*
     * Each conversion, and what its output must declare: the runtime format, the FourCC, the DXGI
     * format (0 without a DX10 header), the bytes of a 4x4 block or of a pixel (the other one 0),
     * and the channels a decode carries.
     */
    static const struct {
        edds_conversion   conversion;
        edds_pixel_format format;
        const char       *four_cc;
        uint32_t          dxgi;
        uint32_t          block_bytes;
        uint32_t          pixel_bytes;
        const char       *channels;
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
        edds_info    info;

        /* Converted under this conversion, stored as COPY. */
        edds_default_profile(&profile);
        profile.conversion      = expected[at].conversion;
        profile.format_compress = EDDS_COMPRESS_COPY;
        CHECK(converted_source(bgra, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, &profile, 0, &info, NULL, NULL));

        /* The runtime format, its FourCC, DXGI format and channels, and a preview. */
        CHECK(info.pixel_format == expected[at].format);
        CHECK(strcmp(info.four_cc, expected[at].four_cc) == 0);
        CHECK(info.dxgi_format == expected[at].dxgi);
        CHECK(strcmp(edds_pixel_format_channels(info.pixel_format), expected[at].channels) == 0);
        CHECK(info.preview_supported);

        /* Four mips: 9x5, 4x2, 2x1 and 1x1, where the chain ends. */
        CHECK(info.mip_count == 4u);

        /* Every level stored as COPY, decoding to its blocks or to its pixels. */
        for (uint32_t level = 0; level < info.mip_count; ++level) {
            const uint32_t width  = info.mips[level].width;
            const uint32_t height = info.mips[level].height;

            const uint32_t bytes = expected[at].block_bytes != 0
                ? block_count_of(width, height) * expected[at].block_bytes
                : width * height * expected[at].pixel_bytes;

            CHECK(info.mips[level].decoded_bytes == bytes);
            CHECK(info.mips[level].container == EDDS_CONTAINER_COPY);
        }

        /* A block format declares its top mip as a linear size; an uncompressed one as a pitch. */
        CHECK(info.pitch_or_linear_size ==
            (expected[at].block_bytes != 0
                    ? block_count_of(GRADIENT_WIDTH, GRADIENT_HEIGHT) * expected[at].block_bytes
                    : GRADIENT_WIDTH * expected[at].pixel_bytes));
    }

    return 1;
}

/**
 * The 9x5 gradient with each alpha shape, under the default profile and under the DXT conversion.
 * No alpha channel gives BGRX and DXT1, an opaque one BGRA and DXT1, and a single sample of 254 or
 * an alpha ramp BGRA and DXT5.
 */
static int the_alpha_branch_is_read_off_each_source(void) {
    /* Each shape, whether the TGA has an alpha channel, and the two formats it must give. */
    static const struct {
        source_alpha_shape shape;
        int                with_alpha;
        edds_pixel_format  uncompressed;
        edds_pixel_format  compressed;
    } cases[] = {
        { SOURCE_NO_ALPHA_CHANNEL, 0, EDDS_PIXEL_BGRX8, EDDS_PIXEL_DXT1 },
        { SOURCE_FULLY_OPAQUE_ALPHA, 1, EDDS_PIXEL_BGRA8, EDDS_PIXEL_DXT1 },
        { SOURCE_ONE_SAMPLE_BELOW_OPAQUE, 1, EDDS_PIXEL_BGRA8, EDDS_PIXEL_DXT5 },
        { SOURCE_ALPHA_RAMP, 1, EDDS_PIXEL_BGRA8, EDDS_PIXEL_DXT5 }
    };

    for (size_t at = 0; at < sizeof cases / sizeof cases[0]; ++at) {
        uint8_t      bgra[GRADIENT_PIXELS * 4];
        edds_profile profile;
        edds_info    info;

        gradient_bgra(bgra, cases[at].shape);

        /* The default profile: uncompressed. */
        edds_default_profile(&profile);
        CHECK(converted_source(bgra, GRADIENT_WIDTH, GRADIENT_HEIGHT, cases[at].with_alpha, &profile, 0, &info, NULL, NULL));
        CHECK(info.pixel_format == cases[at].uncompressed);

        /* The DXT conversion. */
        profile.conversion = EDDS_CONVERSION_DXT;
        CHECK(converted_source(bgra, GRADIENT_WIDTH, GRADIENT_HEIGHT, cases[at].with_alpha, &profile, 0, &info, NULL, NULL));
        CHECK(info.pixel_format == cases[at].compressed);
    }

    return 1;
}

/**
 * Two sources in one batch each keep their own branch; nothing about it is shared.
 * Under one DXT profile, the gradient with opaque alpha gives DXT1 and the one with an alpha ramp
 * gives DXT5.
 */
static int two_sources_under_one_profile_keep_their_own_alpha_branch(void) {
    uint8_t      opaque[GRADIENT_PIXELS * 4];
    uint8_t      translucent[GRADIENT_PIXELS * 4];
    edds_profile profile;
    edds_info    first;
    edds_info    second;

    gradient_bgra(opaque, SOURCE_FULLY_OPAQUE_ALPHA);
    gradient_bgra(translucent, SOURCE_ALPHA_RAMP);

    edds_default_profile(&profile);
    profile.conversion = EDDS_CONVERSION_DXT;
    CHECK(converted_source(opaque, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, &profile, 0, &first, NULL, NULL));
    CHECK(converted_source(translucent, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, &profile, 0, &second, NULL, NULL));
    CHECK(first.pixel_format == EDDS_PIXEL_DXT1 && second.pixel_format == EDDS_PIXEL_DXT5);

    return 1;
}

/**
 * Red and RedGreen store the source channels themselves, so their decode is exact.
 * The 9x5 gradient with an alpha ramp: under Red every pixel decodes to its red alone, under
 * RedGreen to its red and green, with the missing channels 0 and alpha 255.
 */
static int the_uncompressed_channel_formats_are_lossless(void) {
    uint8_t      bgra[GRADIENT_PIXELS * 4];
    uint8_t      expected[GRADIENT_PIXELS * 4];
    edds_profile profile;
    edds_info    info;
    uint8_t     *rgba = NULL;
    size_t       size = 0;

    /* The gradient, and the same as RGBA. */
    gradient_bgra(bgra, SOURCE_ALPHA_RAMP);
    gradient_rgba(bgra, expected, GRADIENT_PIXELS);

    /* The Red conversion. */
    edds_default_profile(&profile);
    profile.conversion = EDDS_CONVERSION_RED;
    CHECK(converted_source(bgra, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, &profile, 0, &info, &rgba, &size));
    CHECK(size == GRADIENT_PIXELS * 4u);

    for (size_t pixel = 0; pixel < GRADIENT_PIXELS; ++pixel) {
        /* What the file holds, not what the source had: green and blue are simply not there. */
        CHECK(rgba[pixel * 4u] == expected[pixel * 4u]);
        CHECK(rgba[pixel * 4u + 1u] == 0 && rgba[pixel * 4u + 2u] == 0);
        CHECK(rgba[pixel * 4u + 3u] == 255u);
    }

    edds_free(rgba);
    rgba = NULL;

    /* The RedGreen conversion. */
    profile.conversion = EDDS_CONVERSION_RED_GREEN;
    CHECK(converted_source(bgra, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, &profile, 0, &info, &rgba, &size));

    for (size_t pixel = 0; pixel < GRADIENT_PIXELS; ++pixel) {
        CHECK(rgba[pixel * 4u] == expected[pixel * 4u]);
        CHECK(rgba[pixel * 4u + 1u] == expected[pixel * 4u + 1u]);
        CHECK(rgba[pixel * 4u + 2u] == 0 && rgba[pixel * 4u + 3u] == 255u);
    }

    edds_free(rgba);

    return 1;
}

/**
 * Every lossy conversion owes a bounded error on the channels it claims to carry.
 * The 16x16 smooth source under the DXT, RedHQ, RedGreenHQ and ColorHQ conversions: the mean
 * error over the channels each carries is at most the bound its case gives.
 */
static int every_lossy_conversion_stays_inside_its_error_bound(void) {
    /* Lists of channels, by their place in RGBA. */
    static const unsigned colour[]       = { 0, 1, 2 };
    static const unsigned colour_alpha[] = { 0, 1, 2, 3 };
    static const unsigned red[]          = { 0 };
    static const unsigned red_green[]    = { 0, 1 };

    /* Each conversion, the channels it carries, and the mean error allowed on them. */
    static const struct {
        edds_conversion conversion;
        const unsigned *channels;
        unsigned        channel_count;
        double          bound;
    } cases[] = {
        { EDDS_CONVERSION_DXT, colour_alpha, 4, 2.5 },
        { EDDS_CONVERSION_RED_HQ, red, 1, 1.0 },
        { EDDS_CONVERSION_RED_GREEN_HQ, red_green, 2, 1.0 },
        { EDDS_CONVERSION_COLOR_HQ, colour_alpha, 4, 2.5 }
    };

    /* The smooth source, and the same as RGBA. */
    uint8_t bgra[SMOOTH_PIXELS * 4];
    uint8_t expected[SMOOTH_PIXELS * 4];

    smooth_bgra(bgra);
    gradient_rgba(bgra, expected, SMOOTH_PIXELS);

    for (size_t at = 0; at < sizeof cases / sizeof cases[0]; ++at) {
        edds_profile profile;
        edds_info    info;
        uint8_t     *rgba = NULL;
        size_t       size = 0;

        edds_default_profile(&profile);
        profile.conversion = cases[at].conversion;
        CHECK(converted_source(bgra, SMOOTH_SIDE, SMOOTH_SIDE, 1, &profile, 0, &info, &rgba, &size));
        CHECK(size == SMOOTH_PIXELS * 4u);
        CHECK(mean_channel_error(rgba, expected, SMOOTH_PIXELS, cases[at].channels, cases[at].channel_count) <= cases[at].bound);
        edds_free(rgba);
    }

    return 1;
}

/**
 * The point of the HQ conversion, stated as a test: given the same opaque colours, BC7 has to come
 * back closer to them than BC1 does. A BC7 encoder that only ever writes one line through a block
 * would pass every other test here and quietly fail this one.
 * The colours are the smooth source's, with every alpha set to 255.
 */
static int the_colour_hq_conversion_beats_dxt_on_the_same_colours(void) {
    /* The channels compared: red, green and blue. */
    static const unsigned colour[] = { 0, 1, 2 };

    /* The source, the same as RGBA, and each conversion's mean error. */
    uint8_t bgra[SMOOTH_PIXELS * 4];
    uint8_t expected[SMOOTH_PIXELS * 4];
    double  dxt_error;
    double  hq_error;

    /* The smooth source, made opaque. */
    smooth_bgra(bgra);

    for (size_t pixel = 0; pixel < SMOOTH_PIXELS; ++pixel) {
        bgra[pixel * 4u + 3u] = 255u;
    }

    gradient_rgba(bgra, expected, SMOOTH_PIXELS);

    {
        edds_profile profile;
        edds_info    info;
        uint8_t     *rgba = NULL;
        size_t       size = 0;

        /* The DXT conversion, which gives DXT1 here. */
        edds_default_profile(&profile);
        profile.conversion = EDDS_CONVERSION_DXT;
        CHECK(converted_source(bgra, SMOOTH_SIDE, SMOOTH_SIDE, 1, &profile, 0, &info, &rgba, &size));
        CHECK(info.pixel_format == EDDS_PIXEL_DXT1);
        dxt_error = mean_channel_error(rgba, expected, SMOOTH_PIXELS, colour, 3);
        edds_free(rgba);
        rgba = NULL;

        /* The ColorHQ conversion, which gives BC7. */
        profile.conversion = EDDS_CONVERSION_COLOR_HQ;
        CHECK(converted_source(bgra, SMOOTH_SIDE, SMOOTH_SIDE, 1, &profile, 0, &info, &rgba, &size));
        CHECK(info.pixel_format == EDDS_PIXEL_BC7);
        hq_error = mean_channel_error(rgba, expected, SMOOTH_PIXELS, colour, 3);
        edds_free(rgba);
    }

    CHECK(hq_error < dxt_error);

    return 1;
}

/**
 * ConversionQuality where nothing shows it has an effect. For None, Red and RedGreen a quality of
 * 0.5 is refused and the default, 1, passes; for the compressed conversions 0 and 1 pass and 1.001
 * is refused. HDRCompression is refused by name, and an unknown conversion is refused.
 */
static int conversion_quality_is_refused_where_nothing_proves_an_effect(void) {
    /* The conversions that store samples as they are, and the compressed ones. */
    static const edds_conversion uncompressed[] = {
        EDDS_CONVERSION_NONE, EDDS_CONVERSION_RED, EDDS_CONVERSION_RED_GREEN
    };
    static const edds_conversion compressed[] = {
        EDDS_CONVERSION_DXT, EDDS_CONVERSION_RED_HQ,
        EDDS_CONVERSION_RED_GREEN_HQ, EDDS_CONVERSION_COLOR_HQ
    };

    edds_profile profile;
    edds_error   error;

    /* The uncompressed conversions: 0.5 refused, the default accepted. */
    for (size_t at = 0; at < sizeof uncompressed / sizeof uncompressed[0]; ++at) {
        edds_default_profile(&profile);
        profile.conversion         = uncompressed[at];
        profile.conversion_quality = EDDS_QUALITY_SCALE / 2u;
        CHECK(refused_profile(&profile, &error) == EDDS_UNSUPPORTED_FORMAT);
        CHECK(strcmp(error.code, "unsupported-setting") == 0);

        /* The default is not a combination; it is what every conversion already has. */
        profile.conversion_quality = EDDS_QUALITY_SCALE;
        CHECK(edds_profile_check(&profile, &error) == EDDS_OK);
    }

    /* The compressed conversions: 0 and 1 accepted, 1.001 refused. */
    for (size_t at = 0; at < sizeof compressed / sizeof compressed[0]; ++at) {
        edds_default_profile(&profile);
        profile.conversion         = compressed[at];
        profile.conversion_quality = 0;
        CHECK(edds_profile_check(&profile, &error) == EDDS_OK);
        profile.conversion_quality = EDDS_QUALITY_SCALE;
        CHECK(edds_profile_check(&profile, &error) == EDDS_OK);
        profile.conversion_quality = EDDS_QUALITY_SCALE + 1u;
        CHECK(refused_profile(&profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    }

    /* HDRCompression, refused by name. */
    edds_default_profile(&profile);
    profile.conversion = EDDS_CONVERSION_HDR;
    CHECK(refused_profile(&profile, &error) == EDDS_UNSUPPORTED_FORMAT);
    CHECK(strstr(error.message, "HDRCompression") != NULL);

    /* A conversion that does not exist. */
    edds_default_profile(&profile);
    profile.conversion = (edds_conversion)99;
    CHECK(refused_profile(&profile, &error) == EDDS_UNSUPPORTED_FORMAT);

    return 1;
}

/**
 * Quality has to buy something, or it would be a control that changes nothing.
 * The 9x5 gradient with an alpha ramp under each compressed conversion, at quality 0 and at 1:
 * the same decoded size, and a smaller mean error over all four channels at 1.
 */
static int conversion_quality_changes_a_compressed_result(void) {
    /* The compressed conversions, and the channels compared: all four. */
    static const edds_conversion compressed[] = {
        EDDS_CONVERSION_DXT, EDDS_CONVERSION_RED_HQ,
        EDDS_CONVERSION_RED_GREEN_HQ, EDDS_CONVERSION_COLOR_HQ
    };
    static const unsigned channels[] = { 0, 1, 2, 3 };

    /* The gradient, and the same as RGBA. */
    uint8_t bgra[GRADIENT_PIXELS * 4];
    uint8_t expected[GRADIENT_PIXELS * 4];

    gradient_bgra(bgra, SOURCE_ALPHA_RAMP);
    gradient_rgba(bgra, expected, GRADIENT_PIXELS);

    for (size_t at = 0; at < sizeof compressed / sizeof compressed[0]; ++at) {
        /* The conversion settings, and the output at quality 0 ("cheap") and at 1 ("dear"). */
        edds_profile profile;
        edds_info    cheap;
        edds_info    dear;

        /* Each output's top level, decoded, and its mean error. */
        uint8_t *cheap_rgba = NULL;
        uint8_t *dear_rgba  = NULL;
        size_t   cheap_size = 0;
        size_t   dear_size  = 0;
        double   cheap_error;
        double   dear_error;

        /* At quality 0. */
        edds_default_profile(&profile);
        profile.conversion         = compressed[at];
        profile.conversion_quality = 0;
        CHECK(converted_source(bgra, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, &profile, 0, &cheap, &cheap_rgba, &cheap_size));

        /* At quality 1. */
        profile.conversion_quality = EDDS_QUALITY_SCALE;
        CHECK(converted_source(bgra, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, &profile, 0, &dear, &dear_rgba, &dear_size));

        /* The same decoded size, and less error at quality 1. */
        cheap_error = mean_channel_error(cheap_rgba, expected, GRADIENT_PIXELS, channels, 4);
        dear_error  = mean_channel_error(dear_rgba, expected, GRADIENT_PIXELS, channels, 4);
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
 * The 16x16 quadrant source goes through every conversion here, stored both ways.
 */
static int container_compression_never_changes_a_decoded_pixel(void) {
    /* The conversions: all but HDRCompression. */
    static const edds_conversion conversions[] = {
        EDDS_CONVERSION_NONE, EDDS_CONVERSION_DXT, EDDS_CONVERSION_RED,
        EDDS_CONVERSION_RED_HQ, EDDS_CONVERSION_RED_GREEN,
        EDDS_CONVERSION_RED_GREEN_HQ, EDDS_CONVERSION_COLOR_HQ
    };

    uint8_t bgra[SMOOTH_PIXELS * 4];

    quadrant_bgra(bgra);

    for (size_t at = 0; at < sizeof conversions / sizeof conversions[0]; ++at) {
        /* The conversion settings, and the output stored as COPY and as LZ4. */
        edds_profile profile;
        edds_info    copied;
        edds_info    compressed;

        /* Each output's top level, decoded. */
        uint8_t *copied_rgba     = NULL;
        uint8_t *compressed_rgba = NULL;
        size_t   copied_size     = 0;
        size_t   compressed_size = 0;

        /* Stored as COPY. */
        edds_default_profile(&profile);
        profile.conversion      = conversions[at];
        profile.format_compress = EDDS_COMPRESS_COPY;
        CHECK(converted_source(bgra, SMOOTH_SIDE, SMOOTH_SIDE, 1, &profile, 0, &copied, &copied_rgba, &copied_size));

        /* Stored as LZ4: BEST compression at a threshold of 100. */
        profile.format_compress    = EDDS_COMPRESS_BEST;
        profile.compress_threshold = 100;
        CHECK(converted_source(bgra, SMOOTH_SIDE, SMOOTH_SIDE, 1, &profile, 0, &compressed, &compressed_rgba, &compressed_size));

        /* The same format, and the same decoded bytes. */
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
 * The 9x5 gradient goes through each block conversion: four levels, the top one six blocks and
 * every other one a single block, and each level previews at its own size.
 */
static int block_padding_reaches_the_smallest_mip(void) {
    /* The block conversions: RedHQ's blocks are 8 bytes, the others' 16. */
    static const edds_conversion conversions[] = {
        EDDS_CONVERSION_DXT, EDDS_CONVERSION_RED_HQ,
        EDDS_CONVERSION_RED_GREEN_HQ, EDDS_CONVERSION_COLOR_HQ
    };

    uint8_t bgra[GRADIENT_PIXELS * 4];

    gradient_bgra(bgra, SOURCE_ALPHA_RAMP);

    for (size_t at = 0; at < sizeof conversions / sizeof conversions[0]; ++at) {
        edds_profile profile;
        edds_info    info;

        edds_default_profile(&profile);
        profile.conversion = conversions[at];
        CHECK(converted_source(bgra, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, &profile, 0, &info, NULL, NULL));

        /* Four levels, from 9x5 down to 1x1. */
        CHECK(info.mip_count == 4u);
        CHECK(info.mips[0].width == 9 && info.mips[0].height == 5);
        CHECK(info.mips[1].width == 4 && info.mips[1].height == 2);
        CHECK(info.mips[2].width == 2 && info.mips[2].height == 1);
        CHECK(info.mips[3].width == 1 && info.mips[3].height == 1);

        /* Three block columns and two block rows carry a nine-by-five image. */
        CHECK(info.mips[0].decoded_bytes == 6u * (conversions[at] == EDDS_CONVERSION_RED_HQ ? 8u : 16u));

        /* Every smaller level is a single block. */
        for (uint32_t level = 1; level < info.mip_count; ++level) {
            CHECK(info.mips[level].decoded_bytes == (conversions[at] == EDDS_CONVERSION_RED_HQ ? 8u : 16u));
        }

        /* Each level, converted again and previewed, comes back at its own size. */
        for (uint32_t level = 0; level < info.mip_count; ++level) {
            uint8_t  *rgba = NULL;
            size_t    size = 0;
            edds_info reread;

            CHECK(converted_source(bgra, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, &profile, level, &reread, &rgba, &size));
            CHECK(size == (size_t)reread.mips[level].width * reread.mips[level].height * 4u);
            edds_free(rgba);
        }
    }

    return 1;
}

/**
 * A block payload that is not whole blocks is refused at inspection, before any decode.
 * The 9x5 gradient is converted with ColorHQ, without mips; with its level's stored size lowered
 * by one 16-byte block and the file cut to match, inspection fails with "unexpected-mip-size".
 */
static int truncated_gpu_blocks_are_refused(void) {
    /* The gradient, as BGRA samples and as a TGA. */
    uint8_t bgra[GRADIENT_PIXELS * 4];
    uint8_t tga[18u + GRADIENT_PIXELS * 4u];

    /* The converted file, read into memory. */
    uint8_t *converted      = NULL;
    size_t   converted_size = 0;

    /* The TGA and the converted file, as files. */
    FILE *source;
    FILE *output = temporary();

    /* The conversion settings, a failed call's error, what inspection reads, the file's length. */
    edds_profile profile;
    edds_error   error;
    edds_info    info;
    long         size;

    /* The gradient converted with ColorHQ, without mips, as COPY. */
    gradient_bgra(bgra, SOURCE_ALPHA_RAMP);
    source = stream_of(tga, fixture_tga_build(tga, sizeof tga, GRADIENT_WIDTH, GRADIENT_HEIGHT, 1, bgra));
    edds_default_profile(&profile);
    profile.conversion      = EDDS_CONVERSION_COLOR_HQ;
    profile.format_compress = EDDS_COMPRESS_COPY;
    profile.generate_mips   = 0;
    CHECK(source != NULL && output != NULL);
    CHECK(edds_convert(source, EDDS_SOURCE_TGA, output, &profile, never_cancelled, NULL, NULL, NULL, &error) == EDDS_OK);
    fclose(source);

    /* The whole output read into memory. */
    CHECK(fseek(output, 0, SEEK_END) == 0 && (size = ftell(output)) > 0);
    converted_size = (size_t)size;
    converted      = malloc(converted_size);
    CHECK(converted != NULL && fseek(output, 0, SEEK_SET) == 0);
    CHECK(fread(converted, 1, converted_size, output) == converted_size);
    fclose(output);

    /* One block short of what three by two blocks require, declared in the mip table itself. */
    {
        FILE        *damaged;
        const size_t table_at = 148u;

        /* The level's stored size, in its mip table entry at byte 148, lowered by 16. */
        converted[table_at + 4u] = (uint8_t)(6u * 16u - 16u);

        /* The file cut short by the same 16 bytes. */
        damaged = stream_of(converted, converted_size - 16u);
        CHECK(damaged != NULL);
        CHECK(edds_inspect(damaged, &info, never_cancelled, NULL, &error) == EDDS_INVALID_INPUT);
        CHECK(strcmp(error.code, "unexpected-mip-size") == 0);
        fclose(damaged);
    }

    free(converted);

    return 1;
}

/**
 * Every conversion and every quality the CLI accepts survives a trip through the metadata text.
 * TGA metadata under each of the eight conversions and six qualities: a conversion that is not
 * supported, or a quality other than 1 on one that does not use quality, cannot be written; every
 * other one parses back with the same conversion and quality.
 */
static int every_conversion_round_trips_through_metadata(void) {
    /* The qualities tried, in thousandths. */
    static const uint32_t qualities[] = { 0, 26, 30, 500, 403, EDDS_QUALITY_SCALE };

    /* The conversion table; how many rows it has goes to count. */
    size_t                            count        = 0;
    const edds_conversion_capability *capabilities = edds_conversions(&count);

    CHECK(count == 8u);

    for (size_t at = 0; at < count; ++at) {
        for (size_t quality = 0; quality < sizeof qualities / sizeof qualities[0]; ++quality) {
            edds_metadata metadata;
            edds_metadata parsed;
            edds_error    error;
            FILE         *written;

            /* Metadata for a TGA, under this conversion and quality. */
            memset(&metadata, 0, sizeof metadata);
            memcpy(metadata.guid, "0123456789ABCDEF", 17);
            (void)snprintf(metadata.name, sizeof metadata.name, "Probe/pixel.edds");
            (void)snprintf(metadata.source_file, sizeof metadata.source_file, "pixel.tga");
            metadata.source_format = EDDS_SOURCE_TGA;
            edds_default_profile(&metadata.profile);
            metadata.profile.conversion         = capabilities[at].conversion;
            metadata.profile.conversion_quality = qualities[quality];

            written = temporary();
            CHECK(written != NULL);

            if (!capabilities[at].supported || (!capabilities[at].uses_quality && qualities[quality] != EDDS_QUALITY_SCALE)) {
                /* Canonical metadata is never allowed to record a recipe that cannot be run. */
                CHECK(edds_metadata_write(written, &metadata, &error) != EDDS_OK);
                fclose(written);
                continue;
            }

            /* Written and parsed back: the same conversion and quality. */
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

/**
 * A quality the text cannot state exactly is refused rather than rounded into something else.
 * Each ConversionQuality text in a DXT configuration either parses to the thousandths its case
 * gives, or fails to parse.
 */
static int metadata_quality_text_is_exact_or_refused(void) {
    /* Each text, whether it is accepted, and the thousandths it then gives. */
    static const struct {
        const char *text;
        int         accepted;
        uint32_t    thousandths;
    } cases[] = {
        { "1", 1, 1000 }, { "0", 1, 0 }, { "0.5", 1, 500 }, { "0.403", 1, 403 },
        { "0.026", 1, 26 }, { "1.000", 1, 1000 }, { "0.0260", 0, 0 }, { "1.5", 0, 0 },
        { "2", 0, 0 }, { "0.", 0, 0 }, { ".5", 0, 0 }, { "-1", 0, 0 }, { "0.5x", 0, 0 }
    };

    for (size_t at = 0; at < sizeof cases / sizeof cases[0]; ++at) {
        char          source[256];
        FILE         *input;
        edds_metadata metadata;
        edds_error    error;

        /* The PC configuration of a TGA under the DXT conversion, with the case's quality. */
        (void)snprintf(source, sizeof source,
            "MetaFileClass { Name \"{0123456789ABCDEF}a.edds\" Configurations { "
            "TGAResourceClass PC { SourceFile \"a.tga\" Conversion DXTCompression "
            "ConversionQuality %s } } }",
            cases[at].text);

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

/**
 * The conversion contract is one table, and everything that names a conversion reads it.
 * Each of the eight conversions is found again by its value, its Workbench name and its wire
 * name; a near miss, NULL and an unknown conversion find none.
 */
static int the_conversion_contract_is_one_table(void) {
    size_t                            count        = 0;
    const edds_conversion_capability *capabilities = edds_conversions(&count);

    CHECK(capabilities != NULL && count == 8u);

    for (size_t at = 0; at < count; ++at) {
        CHECK(edds_conversion_capability_of(capabilities[at].conversion) == &capabilities[at]);
        CHECK(edds_conversion_of_workbench_name(capabilities[at].workbench_name) == &capabilities[at]);
        CHECK(edds_conversion_of_wire_name(capabilities[at].wire_name) == &capabilities[at]);
    }

    /* A name that is almost a conversion's, NULL, and a conversion that does not exist. */
    CHECK(edds_conversion_of_workbench_name("DXT") == NULL);
    CHECK(edds_conversion_of_workbench_name(NULL) == NULL);
    CHECK(edds_conversion_of_wire_name("dxt") == NULL);
    CHECK(edds_conversion_capability_of((edds_conversion)99) == NULL);

    return 1;
}

/**
 * A stored DXT1 block is pixels now, so an existing DayZ texture previews instead of refusing.
 * The 4x4 DXT1 fixture, one block of zeros, inspects as DXT1 and previews as opaque black.
 */
static int a_stored_dxt1_block_decodes_to_its_pixels(void) {
    /* The fixture, as a file. */
    test_bytes fixture = fixture_dxt1();
    FILE      *file    = stream_of(fixture.data, fixture.size);

    /* What inspection reads, and the error a failed call fills in. */
    edds_info  info;
    edds_error error;

    /* The previewed level. */
    uint8_t *rgba = NULL;
    size_t   size = 0;

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
 * The 4x4 DXT5 fixture with such endpoints previews as red 170 throughout, opaque.
 */
static int a_dxt5_colour_block_is_never_read_as_punch_through(void) {
    /* The fixture, as a file. */
    test_bytes fixture = fixture_dxt5_low_endpoints();
    FILE      *file    = stream_of(fixture.data, fixture.size);

    /* What inspection reads, and the error a failed call fills in. */
    edds_info  info;
    edds_error error;

    /* The previewed level. */
    uint8_t *rgba = NULL;
    size_t   size = 0;

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

/**
 * A generated atlas never exists as a file, so it enters the same writer straight from memory.
 * A 64x64 RGBA image encoded with BEST compression, a threshold of 100 and no mips: one level of
 * BGRA stored as LZ4, which previews to exactly the same pixels. A width of 0 is refused as
 * invalid, and an encode whose cancellation is already set returns EDDS_CANCELLED.
 */
static int an_in_memory_atlas_encodes_losslessly_without_mips(void) {
    enum {
        SIDE = 64
    };

    /* The image, and the file it is encoded into. */
    static uint8_t pixels[SIDE * SIDE * 4];
    FILE          *output = temporary();

    /* The encoding settings, what inspection reads, and the error a failed call fills in. */
    edds_profile profile;
    edds_info    info;
    edds_error   error;

    /* The level read back. */
    uint8_t *rgba = NULL;
    size_t   size = 0;

    for (size_t at = 0; at < sizeof pixels; at += 4u) {
        /* Mostly zero like the gaps of an atlas, with a band of distinct values to keep. */
        const uint8_t value = at / 4u % SIDE < 8u ? (uint8_t)(at / 4u * 7u) : 0u;

        pixels[at]      = value;
        pixels[at + 1u] = (uint8_t)(255u - value);
        pixels[at + 2u] = (uint8_t)(value / 2u);
        pixels[at + 3u] = 255u;
    }

    /* BEST compression at a threshold of 100, without mips. */
    edds_default_profile(&profile);
    profile.format_compress    = EDDS_COMPRESS_BEST;
    profile.compress_threshold = 100;
    profile.generate_mips      = 0;

    /* Encoded, then read back: one 64x64 BGRA level stored as LZ4, the same pixels. */
    CHECK(output != NULL);
    CHECK(edds_encode_rgba(pixels, SIDE, SIDE, 1, output, &profile, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, never_cancelled, NULL, &error) == EDDS_OK);
    CHECK(info.width == SIDE && info.height == SIDE && info.mip_count == 1);
    CHECK(info.pixel_format == EDDS_PIXEL_BGRA8);
    CHECK(info.mips[0].container == EDDS_CONTAINER_LZ4);
    CHECK(edds_preview(output, &info, 0, never_cancelled, NULL, &rgba, &size, &error) == EDDS_OK);
    CHECK(size == sizeof pixels && memcmp(rgba, pixels, size) == 0);
    edds_free(rgba);
    fclose(output);

    /* A width of 0, and an encode that is cancelled from the start. */
    output = temporary();
    CHECK(output != NULL);
    CHECK(edds_encode_rgba(pixels, 0, SIDE, 1, output, &profile, never_cancelled, NULL, &error) == EDDS_INVALID_INPUT);
    CHECK(edds_encode_rgba(pixels, SIDE, SIDE, 1, output, &profile, always_cancelled, NULL, &error) == EDDS_CANCELLED);
    fclose(output);

    return 1;
}

/** Runs every test in turn and stops at the first failure: exits 0 when all pass, 1 otherwise. */
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
        png_colour_key_is_transparency_and_a_palette_changes_nothing() &&
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
        a_dxt5_colour_block_is_never_read_as_punch_through() &&
        an_in_memory_atlas_encodes_losslessly_without_mips();

    return passed ? 0 : 1;
}
