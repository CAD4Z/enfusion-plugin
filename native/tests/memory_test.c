#include <edds/memory.h>
#include <edds/pool.h>
#include "fixture.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { \
    (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); return 0; \
} } while (0)

typedef struct conversion_input {
    test_bytes source;
    edds_source_format format;
    edds_profile profile;
} conversion_input;

static edds_status convert_source(void *context, edds_error *error) {
    const conversion_input *input = context;
    FILE *source = tmpfile();
    FILE *output = tmpfile();
    edds_status status;
    if (source == NULL || output == NULL) {
        if (source != NULL) (void)fclose(source);
        if (output != NULL) (void)fclose(output);
        return EDDS_INTERNAL_FAILURE;
    }
    (void)fwrite(input->source.data, 1, input->source.size, source);
    rewind(source);
    status = edds_convert(source, input->format, output, &input->profile,
        NULL, NULL, NULL, NULL, error);
    (void)fclose(source);
    (void)fclose(output);
    return status;
}

static edds_status convert_png(void *context, edds_error *error) {
    conversion_input input;
    input.source = *(test_bytes *)context;
    input.format = EDDS_SOURCE_PNG;
    edds_default_profile(&input.profile);
    return convert_source(&input, error);
}

static int a_conversion_cannot_allocate_past_its_quota(void) {
    test_bytes fixture = fixture_png_rgba();
    edds_error error;
    edds_memory_result limited;
    edds_memory_result retried;
    CHECK(fixture.data != NULL);
    limited = edds_memory_run(1u, convert_png, &fixture, &error);
    CHECK(limited.status != EDDS_OK);
    CHECK(limited.required > 1u);
    CHECK(limited.peak <= 1u);
    retried = edds_memory_run(UINT64_MAX, convert_png, &fixture, &error);
    CHECK(retried.status == EDDS_OK);
    CHECK(retried.required == 0u);
    CHECK(retried.peak > 1u);
    fixture_free(fixture);
    return 1;
}

typedef struct conversion_batch {
    test_bytes source;
    edds_status results[8];
} conversion_batch;

static void convert_compressed_item(void *context, uint32_t index, edds_pool *pool) {
    conversion_batch *batch = context;
    edds_error error;
    batch->results[index] = edds_pool_execute(pool, edds_pool_charge_of(batch->source.size),
        convert_png, &batch->source, NULL, NULL, &error);
    if (batch->results[index] != EDDS_OK) (void)fprintf(stderr, "%s\n", error.message);
}

static int compressed_images_finish_even_when_the_initial_charge_is_too_small(void) {
    conversion_batch batch = { 0 };
    edds_error error;
    edds_memory_result measured;
    batch.source = fixture_png_flat(1024u);
    CHECK(batch.source.data != NULL);
    CHECK(batch.source.size < 32768u);
    measured = edds_memory_run(UINT64_MAX, convert_png, &batch.source, &error);
    CHECK(measured.status == EDDS_OK);
    /* Four million decoded samples cannot fit in this compressed source's four-megabyte hint. */
    CHECK(measured.peak > edds_pool_charge_of(batch.source.size));
    CHECK(edds_pool_run(8u, 8u * 1024u * 1024u, convert_compressed_item, &batch, &error) == EDDS_OK);
    for (uint32_t at = 0; at < 8u; ++at) CHECK(batch.results[at] == EDDS_OK);
    fixture_free(batch.source);
    return 1;
}

static int every_codec_unwinds_allocations_when_a_stage_runs_out_of_quota(void) {
    test_bytes fixtures[] = { fixture_png_rgba(), fixture_tga_bgrx(), fixture_jpeg_ycbcr(),
        fixture_tiff_rgb(), fixture_dds_bgrx_mips() };
    const edds_source_format formats[] = {
        EDDS_SOURCE_PNG, EDDS_SOURCE_TGA, EDDS_SOURCE_JPG, EDDS_SOURCE_TIFF, EDDS_SOURCE_DDS
    };
    for (size_t at = 0; at < sizeof formats / sizeof formats[0]; ++at) {
        conversion_input input;
        edds_error error;
        CHECK(fixtures[at].data != NULL);
        input.source = fixtures[at];
        input.format = formats[at];
        for (unsigned mode = 0; mode < 3u; ++mode) {
            edds_memory_result full;
            edds_default_profile(&input.profile);
            if (mode == 1u) input.profile.conversion = EDDS_CONVERSION_COLOR_HQ;
            if (mode == 2u) {
                input.profile.mipmap_filter = EDDS_FILTER_KAISER;
                input.profile.mipmap_function = EDDS_MIPMAP_COLOR_NOISE;
                input.profile.tiled_texture = 0;
            }
            if (mode == 1u && input.format == EDDS_SOURCE_DDS) {
                input.profile.contains_mips = 1;
                input.profile.generate_mips = 0;
            }
            full = edds_memory_run(UINT64_MAX, convert_source, &input, &error);
            CHECK(full.status == EDDS_OK && full.peak > 0u && full.required == 0u);
            for (uint64_t limit = 1u; limit < full.peak; limit *= 2u) {
                const edds_memory_result limited = edds_memory_run(limit, convert_source, &input, &error);
                CHECK(limited.status != EDDS_OK);
                CHECK(strcmp(error.code, "operation-memory-leak") != 0);
                CHECK(limited.required > limit && limited.peak <= limit);
            }
        }
        fixture_free(fixtures[at]);
    }
    return 1;
}

typedef struct cancelled_batch {
    unsigned calls;
    edds_status result;
} cancelled_batch;

static int already_cancelled(void *context) {
    (void)context;
    return 1;
}

static edds_status operation_that_must_not_start(void *context, edds_error *error) {
    cancelled_batch *batch = context;
    (void)error;
    ++batch->calls;
    return EDDS_OK;
}

static void cancelled_item(void *context, uint32_t index, edds_pool *pool) {
    cancelled_batch *batch = context;
    edds_error error;
    (void)index;
    batch->result = edds_pool_execute(pool, 1u, operation_that_must_not_start, batch,
        already_cancelled, NULL, &error);
}

static int cancellation_is_checked_before_an_attempt_starts(void) {
    cancelled_batch batch = { 0, EDDS_OK };
    edds_error error;
    CHECK(edds_pool_run(1u, 0u, cancelled_item, &batch, &error) == EDDS_OK);
    CHECK(batch.result == EDDS_CANCELLED && batch.calls == 0u);
    return 1;
}

int main(void) {
    return a_conversion_cannot_allocate_past_its_quota() &&
        compressed_images_finish_even_when_the_initial_charge_is_too_small() &&
        every_codec_unwinds_allocations_when_a_stage_runs_out_of_quota() &&
        cancellation_is_checked_before_an_attempt_starts() ? 0 : 1;
}
