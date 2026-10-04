#include "fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { EDDS_FIXTURES = 5,
    SOURCE_FIXTURES = 7 };

/* The Workbench capture owns source pixels; no production decoder builds this input. */
static int mip_source(int argc, char **argv) {
    if (argc != 7) {
        return 2;
    }
    const unsigned w = (unsigned)strtoul(argv[3], NULL, 10);
    const unsigned h = (unsigned)strtoul(argv[4], NULL, 10);
    const int alpha = strcmp(argv[5], "1") == 0;
    const size_t size = (size_t)w * h * 4;
    uint8_t pixels[32 * 32 * 4], tga[18 + sizeof pixels];
    if (w == 0 || h == 0 || w > 32 || h > 32 || strlen(argv[6]) != size * 2) {
        return 2;
    }
    for (size_t at = 0; at < size; ++at) {
        char hex[3] = { argv[6][at * 2], argv[6][at * 2 + 1], 0 };
        pixels[at] = (uint8_t)strtoul(hex, NULL, 16);
    }
    const size_t bytes = fixture_tga_build(tga, sizeof tga, w, h, alpha, pixels);
    const test_bytes source = { tga, bytes };
    return bytes != 0 && fixture_write(argv[2], source) ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--mip-source") == 0) {
        return mip_source(argc, argv);
    }
    test_bytes fixtures[EDDS_FIXTURES + SOURCE_FIXTURES];
    const int argc_without_sources = 1 + EDDS_FIXTURES;
    const int argc_with_sources = argc_without_sources + SOURCE_FIXTURES;
    int produced;
    int ok;
    if (argc == 3 && strcmp(argv[1], "--memory") == 0) {
        test_bytes source = fixture_png_flat(1024u);
        ok = source.data != NULL && fixture_write(argv[2], source);
        fixture_free(source);
        return ok ? 0 : 1;
    }
    if (argc != argc_without_sources && argc != argc_with_sources) {
        fputs("usage: edds-fixture COPY LZ4 DXT1 ODD_FOURCC OVERFLOW [PNG TGA JPG TIFF GPU_TGA GPU_FLAT_TGA DDS]\n",
            stderr);
        return 2;
    }

    fixtures[0] = fixture_copy_bgra();
    fixtures[1] = fixture_lz4_bgrx();
    fixtures[2] = fixture_dxt1();
    fixtures[3] = fixture_odd_fourcc();
    fixtures[4] = fixture_integer_overflow();
    produced = EDDS_FIXTURES;
    if (argc == argc_with_sources) {
        fixtures[5] = fixture_png_rgba();
        fixtures[6] = fixture_tga_bgrx();
        fixtures[7] = fixture_jpeg_ycbcr();
        fixtures[8] = fixture_tiff_rgb();
        fixtures[9] = fixture_tga_gpu_gradient();
        fixtures[10] = fixture_tga_gpu_flat();
        fixtures[11] = fixture_dds_bgrx_mips();
        produced = EDDS_FIXTURES + SOURCE_FIXTURES;
    }
    ok = 1;
    for (int at = 0; at < produced; ++at) {
        ok = ok && fixtures[at].data != NULL && fixture_write(argv[at + 1], fixtures[at]);
    }
    for (int at = 0; at < produced; ++at) {
        fixture_free(fixtures[at]);
    }
    return ok ? 0 : 1;
}
