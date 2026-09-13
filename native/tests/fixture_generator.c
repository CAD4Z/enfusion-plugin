#include "fixture.h"

#include <stdio.h>
#include <stdlib.h>

enum { EDDS_FIXTURES = 5, SOURCE_FIXTURES = 6 };

int main(int argc, char **argv) {
    test_bytes fixtures[EDDS_FIXTURES + SOURCE_FIXTURES];
    const int argc_without_sources = 1 + EDDS_FIXTURES;
    const int argc_with_sources = argc_without_sources + SOURCE_FIXTURES;
    int produced;
    int ok;
    if (argc != argc_without_sources && argc != argc_with_sources) {
        fputs("usage: edds-fixture COPY LZ4 DXT1 ODD_FOURCC OVERFLOW [PNG TGA JPG TIFF GPU_TGA GPU_FLAT_TGA]\n",
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
