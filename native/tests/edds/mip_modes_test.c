#include "edds/edds.h"
#include "fixture.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) \
    do { \
        if (!(value)) { \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); \
            return 0; \
        } \
    } while (0)

/* DayZ 1.29.163709 importer fixture: ColorNoise preserves the filtered RGB and alpha. */
static int color_noise_matches_the_observed_mip_function(void) {
    const uint8_t bgra[]     = { 0, 0, 255, 0, 67, 19, 0, 47, 13, 79, 255, 59, 80, 98, 0, 106 };
    const uint8_t expected[] = { 128, 49, 40, 53 };
    uint8_t       tga[18 + sizeof bgra];
    const size_t  size = fixture_tga_build(tga, sizeof tga, 2, 2, 1, bgra);
    edds_profile  profile;
    edds_error    error;
    edds_info     info;
    uint8_t      *rgba      = NULL;
    size_t        rgba_size = 0;
    FILE         *source    = tmpfile();
    FILE         *output    = tmpfile();
    CHECK(size != 0 && source != NULL && output != NULL);
    CHECK(fwrite(tga, 1, size, source) == size && fseek(source, 0, SEEK_SET) == 0);
    edds_default_profile(&profile);
    profile.mipmap_function = EDDS_MIPMAP_COLOR_NOISE;
    CHECK(edds_convert(source, EDDS_SOURCE_TGA, output, &profile,
              NULL, NULL, NULL, NULL, &error) == EDDS_OK);
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

static int untiled_kaiser_clamps_edges(void) {
    const uint8_t expected[] =
        "\x2b\x35\x98\x3a\xa5\x58\x66\x8d\x3f\x7d\xa9\xa9\xb7\x9c\x6e\x55"
        "\x45\x8b\xa3\xa1\xbc\x75\x7b\x54\x58\x95\xbd\x6e\xcc\x4d\x75\xb9"
        "\x5d\x70\xad\x68\x9c\x8c\x8f\x7f\x6a\xaf\xcb\x94\x70\x9a\x14\x48"
        "\x76\x83\xb8\x96\x7d\x3c\xa3\xad\x80\x5e\xa7\x5d\x84\x7e\x18\xad";
    uint8_t bgra[8 * 8 * 4];
    uint8_t tga[18 + sizeof bgra];
    for (unsigned y = 0; y < 8; ++y) {
        for (unsigned x = 0; x < 8; ++x) {
            uint8_t *p = bgra + (y * 8 + x) * 4;
            p[0]       = (uint8_t)((x * 67 + y * 13) % 256);
            p[1]       = (uint8_t)((y * 79 + x * 19) % 256);
            p[2]       = (uint8_t)(x == 0 ? 255 : x == 7 ? 0
                                                         : (x * 37 + y * 11) % 256);
            p[3]       = (uint8_t)((x * 47 + y * 59) % 256);
        }
    }
    const size_t size = fixture_tga_build(tga, sizeof tga, 8, 8, 1, bgra);
    edds_profile profile;
    edds_error   error;
    edds_info    info;
    uint8_t     *rgba      = NULL;
    size_t       rgba_size = 0;
    FILE        *source    = tmpfile();
    FILE        *output    = tmpfile();
    CHECK(size != 0 && source != NULL && output != NULL);
    CHECK(fwrite(tga, 1, size, source) == size && fseek(source, 0, SEEK_SET) == 0);
    edds_default_profile(&profile);
    profile.tiled_texture = 0;
    profile.mipmap_filter = EDDS_FILTER_KAISER;
    CHECK(edds_convert(source, EDDS_SOURCE_TGA, output, &profile,
              NULL, NULL, NULL, NULL, &error) == EDDS_OK);
    CHECK(fseek(output, 0, SEEK_SET) == 0);
    CHECK(edds_inspect(output, &info, NULL, NULL, &error) == EDDS_OK);
    CHECK(edds_preview(output, &info, 1, NULL, NULL, &rgba, &rgba_size, &error) == EDDS_OK);
    CHECK(rgba_size == sizeof expected - 1);
    for (size_t at = 0; at < rgba_size; at += 4) {
        CHECK(rgba[at] == expected[at + 2] && rgba[at + 1] == expected[at + 1] &&
            rgba[at + 2] == expected[at] && rgba[at + 3] == expected[at + 3]);
    }
    edds_free(rgba);
    fclose(source);
    fclose(output);
    return 1;
}

int main(void) {
    return color_noise_matches_the_observed_mip_function() && untiled_kaiser_clamps_edges() ? 0 : 1;
}
