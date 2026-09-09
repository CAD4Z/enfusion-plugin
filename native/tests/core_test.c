#include <edds/edds.h>

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
        integer_boundaries_are_refused_before_arithmetic();

    return passed ? 0 : 1;
}
