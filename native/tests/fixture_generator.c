#include "fixture.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    test_bytes copy;
    test_bytes lz4;
    test_bytes dxt1;
    test_bytes odd_fourcc;
    test_bytes overflow;
    int ok;
    if (argc != 6) {
        fputs("usage: edds-fixture COPY_PATH LZ4_PATH DXT1_PATH ODD_FOURCC_PATH OVERFLOW_PATH\n", stderr);
        return 2;
    }

    copy = fixture_copy_bgra();
    lz4 = fixture_lz4_bgrx();
    dxt1 = fixture_dxt1();
    odd_fourcc = fixture_odd_fourcc();
    overflow = fixture_integer_overflow();
    ok = copy.data != NULL && lz4.data != NULL && dxt1.data != NULL &&
        odd_fourcc.data != NULL && overflow.data != NULL &&
        fixture_write(argv[1], copy) && fixture_write(argv[2], lz4) &&
        fixture_write(argv[3], dxt1) && fixture_write(argv[4], odd_fourcc) &&
        fixture_write(argv[5], overflow);
    fixture_free(copy);
    fixture_free(lz4);
    fixture_free(dxt1);
    fixture_free(odd_fourcc);
    fixture_free(overflow);
    return ok ? 0 : 1;
}
