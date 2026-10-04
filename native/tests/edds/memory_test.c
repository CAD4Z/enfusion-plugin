#include <edds/memory.h>
#include <edds/pool.h>
#include "fixture.h"

#include <stdio.h>
#include <string.h>

/** Ends the running test with 0 when a check fails, after printing its file, line and text. */
#define CHECK(value) \
    do { \
        if (!(value)) { \
            (void)fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); \
            return 0; \
        } \
    } while (0)

/** One conversion to run under a quota: the source file's bytes, their format and the profile. */
typedef struct conversion_input {
    test_bytes         source;
    edds_source_format format;
    edds_profile       profile;
} conversion_input;

/**
 * Converts the conversion_input that context points to, from a temporary file holding the source
 * into another temporary file, which is closed unread. Returns the conversion's status, or
 * EDDS_INTERNAL_FAILURE when a temporary file cannot be opened.
 */
static edds_status convert_source(void *context, edds_error *error) {
    const conversion_input *input = context;

    FILE       *source = tmpfile();
    FILE       *output = tmpfile();
    edds_status status;

    if (source == NULL || output == NULL) {
        if (source != NULL) {
            (void)fclose(source);
        }

        if (output != NULL) {
            (void)fclose(output);
        }

        return EDDS_INTERNAL_FAILURE;
    }

    /* The source bytes written out, then converted from the start of the file. */
    (void)fwrite(input->source.data, 1, input->source.size, source);
    rewind(source);
    status = edds_convert(source, input->format, output, &input->profile, NULL, NULL, NULL, NULL, error);

    (void)fclose(source);
    (void)fclose(output);

    return status;
}

/**
 * Converts the PNG that context points to, a test_bytes, with the default profile. Returns what
 * convert_source returns.
 */
static edds_status convert_png(void *context, edds_error *error) {
    conversion_input input;

    input.source = *(test_bytes *)context;
    input.format = EDDS_SOURCE_PNG;
    edds_default_profile(&input.profile);

    return convert_source(&input, error);
}

/**
 * A 3x2 RGBA PNG converted under a one-byte quota must fail, refused by the quota and never holding
 * more than that byte; converted without a limit, it must succeed.
 */
static int a_conversion_cannot_allocate_past_its_quota(void) {
    test_bytes         fixture = fixture_png_rgba();
    edds_error         error;
    edds_memory_result limited;
    edds_memory_result retried;

    CHECK(fixture.data != NULL);

    /* With a quota of one byte: refused, needing more than that byte, and never holding more. */
    limited = edds_memory_run(1u, convert_png, &fixture, &error);
    CHECK(limited.status != EDDS_OK);
    CHECK(limited.required > 1u);
    CHECK(limited.peak <= 1u);

    /* Without a limit: converted, with nothing refused by the quota. */
    retried = edds_memory_run(UINT64_MAX, convert_png, &fixture, &error);
    CHECK(retried.status == EDDS_OK);
    CHECK(retried.required == 0u);
    CHECK(retried.peak > 1u);

    fixture_free(fixture);

    return 1;
}

/** Eight conversions of one source over the worker pool, and the status each one ended with. */
typedef struct conversion_batch {
    test_bytes  source;
    edds_status results[8];
} conversion_batch;

/**
 * One task of the pool: converts the batch's PNG under a quota that starts at the pool's charge
 * for a source of its size, and stores the status at the task's index. A failure's message is
 * printed.
 */
static void convert_compressed_item(void *context, uint32_t index, edds_pool *pool) {
    conversion_batch *batch = context;
    edds_error        error;

    batch->results[index] = edds_pool_execute(pool, edds_pool_charge_of(batch->source.size),
        convert_png, &batch->source, NULL, NULL, &error);

    if (batch->results[index] != EDDS_OK) {
        (void)fprintf(stderr, "%s\n", error.message);
    }
}

/**
 * A flat 1024x1024 PNG that compresses to under 32 KiB needs more memory than the pool's first
 * charge for its size; converted eight times over the pool, every conversion must still succeed.
 */
static int compressed_images_finish_even_when_the_initial_charge_is_too_small(void) {
    conversion_batch   batch = { 0 };
    edds_error         error;
    edds_memory_result measured;

    batch.source = fixture_png_flat(1024u);
    CHECK(batch.source.data != NULL);
    CHECK(batch.source.size < 32768u);

    /* One conversion without a limit, to measure its peak. */
    measured = edds_memory_run(UINT64_MAX, convert_png, &batch.source, &error);
    CHECK(measured.status == EDDS_OK);

    /* Four million decoded samples cannot fit in this compressed source's four-megabyte hint. */
    CHECK(measured.peak > edds_pool_charge_of(batch.source.size));

    /* Eight conversions over the pool, sharing a budget of 8 MiB, all succeed. */
    CHECK(edds_pool_run(8u, 8u * 1024u * 1024u, convert_compressed_item, &batch, &error) == EDDS_OK);

    for (uint32_t at = 0; at < 8u; ++at) {
        CHECK(batch.results[at] == EDDS_OK);
    }

    fixture_free(batch.source);

    return 1;
}

/**
 * One source of each format, converted under three profiles. Without a limit every conversion
 * succeeds; under every power-of-two quota below its peak it fails, refused by the quota, never
 * holding more than the quota, and with its memory released rather than reported as a leak.
 */
static int every_codec_unwinds_allocations_when_a_stage_runs_out_of_quota(void) {
    /* One source of each format, and that format. */
    test_bytes fixtures[] = {
        fixture_png_rgba(), fixture_tga_bgrx(), fixture_jpeg_ycbcr(), fixture_tiff_rgb(),
        fixture_dds_bgrx_mips()
    };
    const edds_source_format formats[] = {
        EDDS_SOURCE_PNG, EDDS_SOURCE_TGA, EDDS_SOURCE_JPG, EDDS_SOURCE_TIFF, EDDS_SOURCE_DDS
    };

    for (size_t at = 0; at < sizeof formats / sizeof formats[0]; ++at) {
        conversion_input input;
        edds_error       error;

        CHECK(fixtures[at].data != NULL);
        input.source = fixtures[at];
        input.format = formats[at];

        /* Mode 0 is the default profile, 1 ColorHQCompression, 2 untiled Kaiser ColorNoise mips. */
        for (unsigned mode = 0; mode < 3u; ++mode) {
            edds_memory_result full;

            edds_default_profile(&input.profile);

            if (mode == 1u) {
                input.profile.conversion = EDDS_CONVERSION_COLOR_HQ;
            }

            if (mode == 2u) {
                input.profile.mipmap_filter   = EDDS_FILTER_KAISER;
                input.profile.mipmap_function = EDDS_MIPMAP_COLOR_NOISE;
                input.profile.tiled_texture   = 0;
            }

            /* In mode 1 the DDS supplies its own mips rather than having them generated. */
            if (mode == 1u && input.format == EDDS_SOURCE_DDS) {
                input.profile.contains_mips = 1;
                input.profile.generate_mips = 0;
            }

            /* Without a limit: converted, with nothing refused by the quota. */
            full = edds_memory_run(UINT64_MAX, convert_source, &input, &error);
            CHECK(full.status == EDDS_OK && full.peak > 0u && full.required == 0u);

            /* With each power of two below the peak as the quota: refused, within it, no leak. */
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

/**
 * A batch whose one task is cancelled from the start: how many times its operation ran, and the
 * status the task got.
 */
typedef struct cancelled_batch {
    unsigned    calls;
    edds_status result;
} cancelled_batch;

/** A cancellation check that is always true. */
static int already_cancelled(void *context) {
    (void)context;

    return 1;
}

/** Counts its runs in the cancelled_batch that context points to, and succeeds. */
static edds_status operation_that_must_not_start(void *context, edds_error *error) {
    cancelled_batch *batch = context;

    (void)error;
    ++batch->calls;

    return EDDS_OK;
}

/**
 * The batch's one task: executes the counting operation over the pool with a cancellation check
 * that is already true, and stores the status in the batch.
 */
static void cancelled_item(void *context, uint32_t index, edds_pool *pool) {
    cancelled_batch *batch = context;
    edds_error       error;

    (void)index;
    batch->result = edds_pool_execute(pool, 1u, operation_that_must_not_start, batch, already_cancelled, NULL, &error);
}

/**
 * One task run over the pool with its cancellation already set: the pool run succeeds, the task
 * gets EDDS_CANCELLED, and its operation never runs.
 */
static int cancellation_is_checked_before_an_attempt_starts(void) {
    cancelled_batch batch = { 0, EDDS_OK };
    edds_error      error;

    CHECK(edds_pool_run(1u, 0u, cancelled_item, &batch, &error) == EDDS_OK);
    CHECK(batch.result == EDDS_CANCELLED && batch.calls == 0u);

    return 1;
}

/**
 * A flat 32x32 PNG converted with each of the ten swizzles, its largest level removed and
 * normalize set. Under every power-of-two quota below its peak the conversion fails without a
 * leak, and so does every conversion of the source cut short to a multiple of 13 bytes.
 */
static int swizzling_unwinds_partial_buffers_and_rejects_short_channels(void) {
    test_bytes fixture = fixture_png_flat(32u);
    size_t     count   = 0;

    /* The swizzles the converter supports; how many there are goes to count. */
    const edds_swizzle_capability *mappings = edds_swizzles(&count);

    CHECK(fixture.data != NULL && count == 10u);

    for (size_t at = 0; at < count; ++at) {
        conversion_input input;
        edds_error       error;

        /* The PNG, under the default profile. */
        edds_default_profile(&input.profile);
        input.source = fixture;
        input.format = EDDS_SOURCE_PNG;

        /* Changed from the default: this swizzle, the largest level removed, and normalize set. */
        input.profile.swizzling   = mappings[at].swizzling;
        input.profile.remove_mips = 1u;
        input.profile.normalize   = 1;

        /* Without a limit: converted, with nothing refused by the quota. */
        const edds_memory_result full = edds_memory_run(UINT64_MAX, convert_source, &input, &error);

        CHECK(full.status == EDDS_OK && full.required == 0);

        /* With each power of two below the peak as the quota: refused, within it, no leak. */
        for (uint64_t quota = 1; quota < full.peak; quota *= 2) {
            const edds_memory_result limited = edds_memory_run(quota, convert_source, &input, &error);

            CHECK(limited.status != EDDS_OK && limited.peak <= quota && limited.required > quota);
            CHECK(strcmp(error.code, "operation-memory-leak") != 0);
        }

        /* The source cut short to every multiple of 13 bytes: refused, and no leak. */
        for (size_t length = 0; length < fixture.size; length += 13) {
            input.source.size = length;
            CHECK(edds_memory_run(UINT64_MAX, convert_source, &input, &error).status != EDDS_OK);
            CHECK(strcmp(error.code, "operation-memory-leak") != 0);
        }
    }

    fixture_free(fixture);

    return 1;
}

/** Runs the tests in order and stops at the first failure: exits 0 when all pass, 1 otherwise. */
int main(void) {
    return a_conversion_cannot_allocate_past_its_quota() &&
            compressed_images_finish_even_when_the_initial_charge_is_too_small() &&
            every_codec_unwinds_allocations_when_a_stage_runs_out_of_quota() &&
            swizzling_unwinds_partial_buffers_and_rejects_short_channels() &&
            cancellation_is_checked_before_an_attempt_starts()
        ? 0
        : 1;
}
