#ifndef EDDS_BC6_H
#define EDDS_BC6_H
#include <stdint.h>

/** Decodes one unsigned BC6H block of any mode into 16 RGBA floats in row order; alpha is 1. */
void edds_bc6_decode(const uint8_t block[16], float rgba[64]);

/**
 * Encodes 16 RGBA floats, at least 0, as one mode-11 block; values above 65504 saturate. Quality,
 * 0 to 1000, buys more deterministic endpoint search; nothing is clipped to the LDR range.
 */
void edds_bc6_encode(const float rgba[64], uint32_t quality, uint8_t block[16]);
#endif
