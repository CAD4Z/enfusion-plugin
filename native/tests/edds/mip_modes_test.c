#include "edds/edds.h"
#include "fixture.h"

#include <stdio.h>
#include <string.h>

/** Ends the running test with 0 when a check fails, after printing its file, line and text. */
#define CHECK(value) \
    do { \
        if (!(value)) { \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); \
            return 0; \
        } \
    } while (0)

/**
 * DayZ 1.29.163709 importer fixture: ColorNoise preserves the filtered RGB and alpha.
 * A 2x2 TGA with alpha is converted with the ColorNoise mip function, and its 1x1 level must come
 * back as exactly the expected RGBA.
 */
static int color_noise_matches_the_observed_mip_function(void) {
    /* The 2x2 source as BGRA samples, and the RGBA its 1x1 level must hold. */
    const uint8_t bgra[]     = { 0, 0, 255, 0, 67, 19, 0, 47, 13, 79, 255, 59, 80, 98, 0, 106 };
    const uint8_t expected[] = { 128, 49, 40, 53 };

    /* The source as a TGA file in memory, and its size: 0 if it could not be built. */
    uint8_t      tga[18 + sizeof bgra];
    const size_t size = fixture_tga_build(tga, sizeof tga, 2, 2, 1, bgra);

    /* The conversion settings, the error a failed call fills in, and the inspected output. */
    edds_profile profile;
    edds_error   error;
    edds_info    info;

    /* The decoded level. */
    uint8_t *rgba      = NULL;
    size_t   rgba_size = 0;

    /* The source and the converted file, as temporary files. */
    FILE *source = tmpfile();
    FILE *output = tmpfile();

    /* The TGA written to the source file, rewound for reading. */
    CHECK(size != 0 && source != NULL && output != NULL);
    CHECK(fwrite(tga, 1, size, source) == size && fseek(source, 0, SEEK_SET) == 0);

    /* Converted with the default profile, except for the ColorNoise mip function. */
    edds_default_profile(&profile);
    profile.mipmap_function = EDDS_MIPMAP_COLOR_NOISE;
    CHECK(edds_convert(source, EDDS_SOURCE_TGA, output, &profile, NULL, NULL, NULL, NULL, &error) == EDDS_OK);

    /* The output holds two levels, 2x2 and 1x1, and level 1 decodes to the expected RGBA. */
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, NULL, NULL, &error) == EDDS_OK);
    CHECK(info.mip_count == 2);
    CHECK(edds_preview(output, &info, 1, NULL, NULL, &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == sizeof expected && memcmp(rgba, expected, sizeof expected) == 0);

    edds_free(rgba);
    fclose(source);
    fclose(output);

    return 1;
}

/**
 * An 8x8 TGA whose red is 255 down the left column and 0 down the right one, converted untiled
 * with the Kaiser mip filter: its 4x4 level must match the expected bytes exactly.
 */
static int untiled_kaiser_clamps_edges(void) {
    /* The 4x4 level the conversion must produce, in BGRA order, then the string's closing zero. */
    const uint8_t expected[] =
        "\x2b\x35\x98\x3a\xa5\x58\x66\x8d\x3f\x7d\xa9\xa9\xb7\x9c\x6e\x55"
        "\x45\x8b\xa3\xa1\xbc\x75\x7b\x54\x58\x95\xbd\x6e\xcc\x4d\x75\xb9"
        "\x5d\x70\xad\x68\x9c\x8c\x8f\x7f\x6a\xaf\xcb\x94\x70\x9a\x14\x48"
        "\x76\x83\xb8\x96\x7d\x3c\xa3\xad\x80\x5e\xa7\x5d\x84\x7e\x18\xad";

    /* The 8x8 source as BGRA samples, and as a TGA file in memory. */
    uint8_t bgra[8 * 8 * 4];
    uint8_t tga[18 + sizeof bgra];

    /* A pattern of samples, with red 255 down the left column and 0 down the right one. */
    for (unsigned y = 0; y < 8; ++y) {
        for (unsigned x = 0; x < 8; ++x) {
            uint8_t *p = bgra + (y * 8 + x) * 4;

            p[0] = (uint8_t)((x * 67 + y * 13) % 256);
            p[1] = (uint8_t)((y * 79 + x * 19) % 256);
            p[2] = (uint8_t)(x == 0 ? 255 : x == 7 ? 0
                                                   : (x * 37 + y * 11) % 256);
            p[3] = (uint8_t)((x * 47 + y * 59) % 256);
        }
    }

    /* The TGA file's size: 0 if it could not be built. */
    const size_t size = fixture_tga_build(tga, sizeof tga, 8, 8, 1, bgra);

    /* The conversion settings, the error a failed call fills in, and the inspected output. */
    edds_profile profile;
    edds_error   error;
    edds_info    info;

    /* The decoded level. */
    uint8_t *rgba      = NULL;
    size_t   rgba_size = 0;

    /* The source and the converted file, as temporary files. */
    FILE *source = tmpfile();
    FILE *output = tmpfile();

    /* The TGA written to the source file, rewound for reading. */
    CHECK(size != 0 && source != NULL && output != NULL);
    CHECK(fwrite(tga, 1, size, source) == size && fseek(source, 0, SEEK_SET) == 0);

    /* Converted untiled, with the Kaiser mip filter. */
    edds_default_profile(&profile);
    profile.tiled_texture = 0;
    profile.mipmap_filter = EDDS_FILTER_KAISER;
    CHECK(edds_convert(source, EDDS_SOURCE_TGA, output, &profile, NULL, NULL, NULL, NULL, &error) == EDDS_OK);

    /* Level 1 decodes to exactly the expected 4x4 level, each RGBA pixel against its BGRA bytes. */
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, NULL, NULL, &error) == EDDS_OK);
    CHECK(edds_preview(output, &info, 1, NULL, NULL, &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == sizeof expected - 1);

    for (size_t at = 0; at < rgba_size; at += 4) {
        CHECK(rgba[at] == expected[at + 2] &&
            rgba[at + 1] == expected[at + 1] &&
            rgba[at + 2] == expected[at] &&
            rgba[at + 3] == expected[at + 3]);
    }

    edds_free(rgba);
    fclose(source);
    fclose(output);

    return 1;
}

/** Runs the tests in order and stops at the first failure: exits 0 when all pass, 1 otherwise. */
int main(void) {
    return color_noise_matches_the_observed_mip_function() && untiled_kaiser_clamps_edges() ? 0 : 1;
}
