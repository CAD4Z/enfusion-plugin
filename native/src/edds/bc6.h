#ifndef EDDS_BC6_H
#define EDDS_BC6_H
#include <stdint.h>

/* Unsigned BC6H: RGB floats in row order, alpha is always one. All 14 modes decode. */
void edds_bc6_decode(const uint8_t block[16], float rgba[64]);
/* Mode 11 encoder. Quality adds deterministic endpoint refinement; never clips to LDR. */
void edds_bc6_encode(const float rgba[64], uint32_t quality, uint8_t block[16]);
#endif
