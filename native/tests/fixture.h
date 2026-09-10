#ifndef EDDS_TEST_FIXTURE_H
#define EDDS_TEST_FIXTURE_H

#include <stddef.h>
#include <stdint.h>

typedef struct test_bytes {
    uint8_t *data;
    size_t size;
} test_bytes;

test_bytes fixture_copy_bgra(void);
test_bytes fixture_lz4_bgrx(void);
test_bytes fixture_lz4_streaming_bgrx(void);
test_bytes fixture_dx10_bgrx(void);
test_bytes fixture_dxt1(void);
test_bytes fixture_odd_fourcc(void);
test_bytes fixture_integer_overflow(void);
test_bytes fixture_png_rgba(void);
test_bytes fixture_png_rgba_gamma(void);
test_bytes fixture_tga_bgrx(void);
void fixture_free(test_bytes fixture);
int fixture_write(const char *path, test_bytes fixture);

#endif
