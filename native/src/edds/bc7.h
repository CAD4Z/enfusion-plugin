#ifndef EDDS_BC7_H
#define EDDS_BC7_H

#include <stdint.h>

/**
 * BC7 is eight block modes, and a texture written by anything other than this converter may use
 * any of them, so the decoder implements all eight. The encoder writes mode 6 — one subset, four
 * bit indices, full alpha — which is the mode that carries an RGBA block with no subset guess, and
 * spends `refits` least-squares passes on its endpoints.
 */
void edds_bc7_encode_block(const uint8_t bgra[64], unsigned refits, uint8_t block[16]);

/** Decodes one block into 16 RGBA pixels. A block with no mode bit set decodes to zeroes. */
void edds_bc7_decode_block(const uint8_t block[16], uint8_t rgba[64]);

#endif
