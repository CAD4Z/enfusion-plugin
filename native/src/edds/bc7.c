#include "bc7.h"

#include <string.h>

/*
 * The partition, anchor and weight tables are the block layout itself: a decoder that disagrees
 * with them on one entry decodes the wrong pixels for that partition only, which is why they are
 * written out whole rather than derived.
 */

/** The 64 two-subset partitions: for each, the subset of each of the 16 pixels, row by row. */
static const uint8_t partitions_two[64][16] = {
    { 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1 },
    { 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1 },
    { 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1 },
    { 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1 },
    { 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1 },
    { 0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1 },
    { 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1 },
    { 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1 },
    { 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1 },
    { 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0 },
    { 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0 },
    { 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0 },
    { 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1 },
    { 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0 },
    { 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0 },
    { 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0 },
    { 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0 },
    { 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0 },
    { 0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0 },
    { 0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0 },
    { 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1 },
    { 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1 },
    { 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0 },
    { 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0 },
    { 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0 },
    { 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0 },
    { 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1 },
    { 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1 },
    { 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0 },
    { 0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0 },
    { 0, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0 },
    { 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0 },
    { 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0 },
    { 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1 },
    { 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1 },
    { 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0 },
    { 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0 },
    { 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0 },
    { 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1 },
    { 0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1 },
    { 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0 },
    { 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0 },
    { 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1 },
    { 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1 },
    { 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1 },
    { 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1 },
    { 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1 },
    { 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0 },
    { 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0 },
    { 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1 }
};

/** The 64 three-subset partitions, laid out the same way. */
static const uint8_t partitions_three[64][16] = {
    { 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 2 },
    { 0, 0, 0, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1 },
    { 0, 0, 0, 0, 2, 0, 0, 1, 2, 2, 1, 1, 2, 2, 1, 1 },
    { 0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 1, 0, 1, 1, 1 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2 },
    { 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2 },
    { 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1 },
    { 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2 },
    { 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2 },
    { 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2 },
    { 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2 },
    { 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2 },
    { 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2 },
    { 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 2 },
    { 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0, 2, 2, 2, 0 },
    { 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2 },
    { 0, 1, 1, 1, 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0 },
    { 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2 },
    { 0, 0, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1 },
    { 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2, 0, 2, 2, 2 },
    { 0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 1, 2, 2, 2, 1 },
    { 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2 },
    { 0, 0, 0, 0, 1, 1, 0, 0, 2, 2, 1, 0, 2, 2, 1, 0 },
    { 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0 },
    { 0, 0, 1, 2, 0, 0, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2 },
    { 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 1, 1, 0 },
    { 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1 },
    { 0, 0, 2, 2, 1, 1, 0, 2, 1, 1, 0, 2, 0, 0, 2, 2 },
    { 0, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 2, 2, 2, 2 },
    { 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1 },
    { 0, 0, 0, 0, 2, 0, 0, 0, 2, 2, 1, 1, 2, 2, 2, 1 },
    { 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 2, 2, 2 },
    { 0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 2, 0, 0, 1, 1 },
    { 0, 0, 1, 1, 0, 0, 1, 2, 0, 0, 2, 2, 0, 2, 2, 2 },
    { 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0 },
    { 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0 },
    { 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0 },
    { 0, 1, 2, 0, 2, 0, 1, 2, 1, 2, 0, 1, 0, 1, 2, 0 },
    { 0, 0, 1, 1, 2, 2, 0, 0, 1, 1, 2, 2, 0, 0, 1, 1 },
    { 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1 },
    { 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1 },
    { 0, 0, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 1, 1, 2, 2 },
    { 0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 1, 1 },
    { 0, 2, 2, 0, 1, 2, 2, 1, 0, 2, 2, 0, 1, 2, 2, 1 },
    { 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 1, 0, 1 },
    { 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1 },
    { 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2 },
    { 0, 2, 2, 2, 0, 1, 1, 1, 0, 2, 2, 2, 0, 1, 1, 1 },
    { 0, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0, 2, 1, 1, 1, 2 },
    { 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2 },
    { 0, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2 },
    { 0, 0, 0, 2, 1, 1, 1, 2, 1, 1, 1, 2, 0, 0, 0, 2 },
    { 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2 },
    { 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2, 2, 2, 2, 2 },
    { 0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2 },
    { 0, 0, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2 },
    { 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1 },
    { 0, 2, 2, 2, 1, 2, 2, 2, 0, 2, 2, 2, 1, 2, 2, 2 },
    { 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 },
    { 0, 1, 1, 1, 2, 0, 1, 1, 2, 2, 0, 1, 2, 2, 2, 0 }
};

/** Per two-subset partition, the anchor pixel of subset 1; subset 0's is always pixel 0. */
static const uint8_t anchor_second_of_two[64] = {
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 15, 15, 15, 15,
    15, 2, 8, 2, 2, 8, 8, 15,
    2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15,
    2, 8, 2, 2, 2, 15, 15, 6,
    6, 2, 6, 8, 15, 15, 2, 2,
    15, 15, 15, 15, 15, 2, 2, 15
};

/** Per three-subset partition, the anchor pixel of subset 1. */
static const uint8_t anchor_second_of_three[64] = {
    3, 3, 15, 15, 8, 3, 15, 15,
    8, 8, 6, 6, 6, 5, 3, 3,
    3, 3, 8, 15, 3, 3, 6, 10,
    5, 8, 8, 6, 8, 5, 15, 15,
    8, 15, 3, 5, 6, 10, 8, 15,
    15, 3, 15, 5, 15, 15, 15, 15,
    3, 15, 5, 5, 5, 8, 5, 10,
    5, 10, 8, 13, 15, 12, 3, 3
};

/** Per three-subset partition, the anchor pixel of subset 2. */
static const uint8_t anchor_third_of_three[64] = {
    15, 8, 8, 3, 15, 15, 3, 8,
    15, 15, 15, 15, 15, 15, 15, 8,
    15, 8, 15, 3, 15, 8, 15, 8,
    3, 15, 6, 10, 15, 15, 10, 8,
    15, 3, 15, 10, 10, 8, 9, 10,
    6, 15, 8, 15, 3, 6, 6, 8,
    15, 3, 15, 15, 15, 15, 15, 15,
    15, 15, 15, 15, 3, 15, 15, 8
};

/**
 * The weights of 2-, 3- and 4-bit indices: how far, in 64ths, each index puts its pixel from the
 * first endpoint towards the second.
 */
static const uint8_t weights_two[4]   = { 0, 21, 43, 64 };
static const uint8_t weights_three[8] = { 0, 9, 18, 27, 37, 46, 55, 64 };
static const uint8_t weights_four[16] = {
    0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 56, 60, 64
};

/**
 * The layout of one mode: the number of subsets; the bits of the partition number, the rotation
 * and the index selection; the bits of each colour and alpha endpoint value; whether there is a
 * P-bit per endpoint or one shared per subset (a P-bit is one more low bit that every channel of
 * an endpoint shares); and the bits of each index, and of each index of a second set.
 */
typedef struct mode_layout {
    uint8_t subsets;
    uint8_t partition_bits;
    uint8_t rotation_bits;
    uint8_t selection_bits;
    uint8_t colour_bits;
    uint8_t alpha_bits;
    uint8_t endpoint_p_bits;
    uint8_t shared_p_bits;
    uint8_t index_bits;
    uint8_t index_bits_two;
} mode_layout;

/** The eight modes, by number. */
static const mode_layout modes[8] = {
    { 3, 4, 0, 0, 4, 0, 1, 0, 3, 0 },
    { 2, 6, 0, 0, 6, 0, 0, 1, 3, 0 },
    { 3, 6, 0, 0, 5, 0, 0, 0, 2, 0 },
    { 2, 6, 0, 0, 7, 0, 1, 0, 2, 0 },
    { 1, 0, 2, 1, 5, 6, 0, 0, 2, 3 },
    { 1, 0, 2, 0, 7, 8, 0, 0, 2, 2 },
    { 1, 0, 0, 0, 7, 7, 1, 0, 4, 0 },
    { 2, 6, 0, 0, 5, 5, 1, 0, 2, 0 }
};

/** Reads a block bit by bit: `at` counts the bits read, from the lowest bit of byte 0 up. */
typedef struct bit_cursor {
    const uint8_t *data;
    unsigned       at;
} bit_cursor;

/** The next `count` bits of the block as a number, the first bit read the lowest. */
static uint32_t take_bits(bit_cursor *cursor, unsigned count) {
    uint32_t value = 0;

    for (unsigned bit = 0; bit < count; ++bit) {
        const unsigned position = cursor->at + bit;

        value |= (uint32_t)((cursor->data[position >> 3] >> (position & 7u)) & 1u) << bit;
    }

    cursor->at += count;

    return value;
}

/**
 * Writes the lowest `count` bits of `value` from bit `*at` on, the lowest first, and moves `*at`
 * past them. It only sets bits, so the block must start zeroed.
 */
static void put_bits(uint8_t *data, unsigned *at, uint32_t value, unsigned count) {
    for (unsigned bit = 0; bit < count; ++bit) {
        const unsigned position = *at + bit;

        if (((value >> bit) & 1u) != 0) {
            data[position >> 3] |= (uint8_t)(1u << (position & 7u));
        }
    }

    *at += count;
}

/** A `bits`-bit endpoint value widened to 8 bits, its top bits repeated below it. */
static uint8_t unquantize(uint32_t value, unsigned bits) {
    if (bits >= 8) {
        return (uint8_t)value;
    }

    return (uint8_t)((value << (8u - bits)) | (value >> (2u * bits - 8u)));
}

/** The value `weight` 64ths of the way from `low` to `high`, rounded. */
static uint8_t interpolate(uint8_t low, uint8_t high, uint8_t weight) {
    return (uint8_t)((((uint32_t)low * (64u - weight)) + ((uint32_t)high * weight) + 32u) >> 6);
}

/** The subset `pixel` belongs to under `partition`: always 0 in a one-subset mode. */
static uint8_t subset_of(const mode_layout *layout, uint32_t partition, unsigned pixel) {
    if (layout->subsets == 1) {
        return 0;
    }

    return layout->subsets == 2 ? partitions_two[partition][pixel] : partitions_three[partition][pixel];
}

/**
 * Whether `pixel` is the anchor of its subset under `partition`; pixel 0 always is. An anchor's
 * index is stored one bit shorter.
 */
static int is_anchor(const mode_layout *layout, uint32_t partition, unsigned pixel) {
    if (pixel == 0) {
        return 1;
    }

    if (layout->subsets == 2) {
        return pixel == anchor_second_of_two[partition];
    }

    if (layout->subsets == 3) {
        return pixel == anchor_second_of_three[partition] || pixel == anchor_third_of_three[partition];
    }

    return 0;
}

/**
 * Decodes one block into 16 RGBA pixels: the mode first, then that mode's fields in the order the
 * block holds them. A block with no mode bit set decodes to zeroes.
 */
void edds_bc7_decode_block(const uint8_t block[16], uint8_t rgba[64]) {
    /* The bits of the block, and the layout of its mode. */
    bit_cursor         cursor = { block, 0 };
    const mode_layout *layout;

    /* The endpoints as RGBA, two per subset, and each pixel's index in the first and second set. */
    uint8_t endpoints[6][4];
    uint8_t indices[16];
    uint8_t indices_two[16];

    /* The partition, rotation and index selection the block names, and its mode. */
    uint32_t partition = 0;
    uint32_t rotation  = 0;
    uint32_t selection = 0;
    unsigned mode      = 0;

    /* The number of endpoints, and the bits of a colour and of an alpha value, P-bit included. */
    unsigned endpoint_count;
    unsigned colour_precision;
    unsigned alpha_precision;

    /* The endpoint values as the block holds them, and their P-bits. */
    uint32_t raw[6][4];
    uint32_t p_bits[6] = { 0, 0, 0, 0, 0, 0 };

    memset(rgba, 0, 64);

    /* The mode is named in unary: as many zero bits as its number, then a one. */
    while (mode < 8 && take_bits(&cursor, 1) == 0) {
        ++mode;
    }

    /*
     * Eight zero bits name no mode at all; hardware reads that block as zeroes, and so does this.
     */
    if (mode >= 8) {
        return;
    }

    layout         = &modes[mode];
    endpoint_count = (unsigned)layout->subsets * 2u;

    partition = take_bits(&cursor, layout->partition_bits);
    rotation  = take_bits(&cursor, layout->rotation_bits);
    selection = take_bits(&cursor, layout->selection_bits);

    /* The endpoint values: red of every endpoint, then green, then blue, then alpha if any. */
    for (unsigned channel = 0; channel < 3; ++channel) {
        for (unsigned endpoint = 0; endpoint < endpoint_count; ++endpoint) {
            raw[endpoint][channel] = take_bits(&cursor, layout->colour_bits);
        }
    }

    for (unsigned endpoint = 0; endpoint < endpoint_count; ++endpoint) {
        raw[endpoint][3] = layout->alpha_bits == 0 ? 0u : take_bits(&cursor, layout->alpha_bits);
    }

    /* The P-bits: one per endpoint, or one per subset that both its endpoints share. */
    if (layout->endpoint_p_bits != 0) {
        for (unsigned endpoint = 0; endpoint < endpoint_count; ++endpoint) {
            p_bits[endpoint] = take_bits(&cursor, 1);
        }
    } else if (layout->shared_p_bits != 0) {
        for (unsigned subset = 0; subset < layout->subsets; ++subset) {
            const uint32_t shared = take_bits(&cursor, 1);

            p_bits[subset * 2u]      = shared;
            p_bits[subset * 2u + 1u] = shared;
        }
    }

    /*
     * The endpoints widened to 8 bits a channel. A value is one bit more precise when the mode has
     * P-bits, which are appended below it; a mode without alpha has opaque endpoints.
     */
    colour_precision = (unsigned)layout->colour_bits + (layout->endpoint_p_bits != 0 || layout->shared_p_bits != 0 ? 1u : 0u);
    alpha_precision  = layout->alpha_bits == 0
         ? 0u
         : (unsigned)layout->alpha_bits +
            (layout->endpoint_p_bits != 0 || layout->shared_p_bits != 0 ? 1u : 0u);

    for (unsigned endpoint = 0; endpoint < endpoint_count; ++endpoint) {
        for (unsigned channel = 0; channel < 3; ++channel) {
            uint32_t value = raw[endpoint][channel];

            if (colour_precision != layout->colour_bits) {
                value = (value << 1) | p_bits[endpoint];
            }

            endpoints[endpoint][channel] = unquantize(value, colour_precision);
        }

        if (layout->alpha_bits == 0) {
            endpoints[endpoint][3] = 255u;
        } else {
            uint32_t value = raw[endpoint][3];

            if (alpha_precision != layout->alpha_bits) {
                value = (value << 1) | p_bits[endpoint];
            }

            endpoints[endpoint][3] = unquantize(value, alpha_precision);
        }
    }

    /* The indices, pixel by pixel; an anchor's is one bit shorter. */
    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        const unsigned bits = (unsigned)layout->index_bits - (is_anchor(layout, partition, pixel) ? 1u : 0u);

        indices[pixel] = (uint8_t)take_bits(&cursor, bits);
    }

    /* The second set of indices, in a mode that has one; otherwise the first set serves again. */
    if (layout->index_bits_two != 0) {
        for (unsigned pixel = 0; pixel < 16; ++pixel) {
            const unsigned bits = (unsigned)layout->index_bits_two - (pixel == 0 ? 1u : 0u);

            indices_two[pixel] = (uint8_t)take_bits(&cursor, bits);
        }
    } else {
        memcpy(indices_two, indices, sizeof indices_two);
    }

    /*
     * Each pixel: its subset's two endpoints mixed by the weight of its index, the first set for
     * the colour and the second for alpha. A selection of 1 swaps the two sets, and a rotation then
     * swaps alpha with red (1), green (2) or blue (3).
     */
    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        /* The pixel's subset, and that subset's two endpoints. */
        const unsigned subset = subset_of(layout, partition, pixel);
        const uint8_t *low    = endpoints[subset * 2u];
        const uint8_t *high   = endpoints[subset * 2u + 1u];

        /* The weights of the colour index, by its bits. */
        const uint8_t *colour_weights = layout->index_bits == 2 ? weights_two : (layout->index_bits == 3 ? weights_three : weights_four);

        /* The weights of the alpha index: the colour's, unless the mode has a second set. */
        const uint8_t *alpha_weights = layout->index_bits_two == 0
            ? colour_weights
            : (layout->index_bits_two == 2 ? weights_two : weights_three);

        /* The pixel's two indices, and the pixel being built. */
        uint8_t colour_index = indices[pixel];
        uint8_t alpha_index  = indices_two[pixel];
        uint8_t out[4];

        if (layout->index_bits_two != 0 && selection != 0) {
            const uint8_t *swap_weights = colour_weights;
            const uint8_t  swap_index   = colour_index;

            colour_weights = alpha_weights;
            alpha_weights  = swap_weights;
            colour_index   = alpha_index;
            alpha_index    = swap_index;
        }

        for (unsigned channel = 0; channel < 3; ++channel) {
            out[channel] = interpolate(low[channel], high[channel], colour_weights[colour_index]);
        }

        out[3] = layout->alpha_bits == 0 ? 255u : interpolate(low[3], high[3], alpha_weights[alpha_index]);

        if (rotation == 1) {
            const uint8_t kept = out[3];

            out[3] = out[0];
            out[0] = kept;
        } else if (rotation == 2) {
            const uint8_t kept = out[3];

            out[3] = out[1];
            out[1] = kept;
        } else if (rotation == 3) {
            const uint8_t kept = out[3];

            out[3] = out[2];
            out[2] = kept;
        }

        memcpy(rgba + pixel * 4u, out, 4);
    }
}

/* ---- Mode 6 encoder: one subset, seven-bit endpoints with a P-bit each, four-bit indices. ---- */

/** The bits that name mode 6: six zeros, then a one. */
enum {
    MODE_SIX_BITS = 7
};

/** `value` held to 0..255. */
static uint8_t clamp_byte(int value) {
    return value < 0 ? 0u : (value > 255 ? 255u : (uint8_t)value);
}

/** The closest value a seven-bit endpoint plus this P-bit can name. */
static uint8_t quantize_with_p(int value, unsigned p) {
    int seven = (value - (int)p + 1) / 2;

    if (seven < 0) {
        seven = 0;
    }

    if (seven > 127) {
        seven = 127;
    }

    return (uint8_t)seven;
}

/** The 16 RGBA colours of mode 6: `low` and `high` mixed by the weight of each 4-bit index. */
static void mode_six_palette(const uint8_t low[4], const uint8_t high[4], uint8_t palette[16][4]) {
    for (unsigned entry = 0; entry < 16; ++entry) {
        for (unsigned channel = 0; channel < 4; ++channel) {
            palette[entry][channel] = interpolate(low[channel], high[channel], weights_four[entry]);
        }
    }
}

/** Each pixel's nearest palette entry, into `indices`; returns the total squared error. */
static uint64_t assign_indices(const uint8_t pixels[16][4], const uint8_t palette[16][4], uint8_t indices[16]) {
    uint64_t total = 0;

    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        uint32_t best   = 0xffffffffu;
        uint8_t  chosen = 0;

        for (unsigned entry = 0; entry < 16; ++entry) {
            uint32_t error = 0;

            for (unsigned channel = 0; channel < 4; ++channel) {
                const int difference = (int)pixels[pixel][channel] - (int)palette[entry][channel];

                error += (uint32_t)(difference * difference);
            }

            if (error < best) {
                best   = error;
                chosen = (uint8_t)entry;
            }
        }

        indices[pixel]  = chosen;
        total          += best;
    }

    return total;
}

/**
 * Moves the endpoints to the least-squares optimum for the indices they produced. It is the whole
 * of what `ConversionQuality` buys here: the bounding box of a block is a first guess, and a block
 * whose samples do not fill that box evenly is encoded measurably better after a pass or two.
 * `low` and `high` are replaced, not yet held to 0..255; when the indices do not pin them down,
 * they stay as they are.
 */
static void refit_endpoints(const uint8_t pixels[16][4], const uint8_t indices[16], int low[4], int high[4]) {
    double a = 0;
    double b = 0;
    double c = 0;

    /*
     * The system least squares solves, with w each pixel's weight as a fraction: a sums
     * (1 - w)^2, b sums (1 - w) * w and c sums w^2.
     */
    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        const double weight = (double)weights_four[indices[pixel]] / 64.0;

        a += (1.0 - weight) * (1.0 - weight);
        b += (1.0 - weight) * weight;
        c += weight * weight;
    }

    /* A determinant of about zero has no single answer, and the endpoints stay as they are. */
    {
        const double determinant = a * c - b * b;

        if (determinant > -1e-9 && determinant < 1e-9) {
            return;
        }

        for (unsigned channel = 0; channel < 4; ++channel) {
            double low_sum  = 0;
            double high_sum = 0;

            for (unsigned pixel = 0; pixel < 16; ++pixel) {
                const double weight = (double)weights_four[indices[pixel]] / 64.0;
                const double sample = (double)pixels[pixel][channel];

                low_sum  += (1.0 - weight) * sample;
                high_sum += weight * sample;
            }

            low[channel]  = (int)((low_sum * c - high_sum * b) / determinant + 0.5);
            high[channel] = (int)((high_sum * a - low_sum * b) / determinant + 0.5);
        }
    }
}

/* ---- The two-subset modes: one partition, two lines through it, one fit each. ---- */

/**
 * What one two-subset mode makes of an endpoint. Mode 1 spends its bits on colour alone and shares
 * one P-bit across each subset; mode 7 carries alpha as well, at fewer bits per channel and with a
 * P-bit of its own per endpoint. Everything else about fitting them is the same.
 */
typedef struct subset_layout {
    /** The mode's number, and how many channels it fits: 3 without alpha, 4 with it. */
    unsigned mode;
    unsigned channels;

    /** The bits of each endpoint value, before its P-bit. */
    unsigned bits;

    /** The index weights, how many there are, and the bits of an index. */
    const uint8_t *weights;
    unsigned       weight_count;
    unsigned       index_bits;

    /** 1 when the two endpoints of a subset share one P-bit. */
    int shared_p;
} subset_layout;

/** Modes 1 and 7, as fit_subset and write_two_subset see them. */
static const subset_layout layout_mode_one   = { 1, 3, 6, weights_three, 8, 3, 1 };
static const subset_layout layout_mode_seven = { 7, 4, 5, weights_two, 4, 2, 0 };

/**
 * The fit of one subset: its two endpoints as the raw fields the block holds, their P-bits, the
 * indices of its pixels (0 for the other pixels), and the squared error.
 */
typedef struct subset_fit {
    unsigned quantized[2][4];
    unsigned p[2];
    uint8_t  indices[16];
    uint64_t error;
} subset_fit;

/** The closest value this many bits plus that P-bit can name, as the raw field the block holds. */
static unsigned quantize_at(int value, unsigned p, unsigned bits) {
    /* The value scaled to the full precision, P-bit included. */
    const unsigned precision = bits + 1u;
    const int      most      = (int)((1u << precision) - 1u);
    const int      scaled    = (value * most + 127) / 255;

    /* The raw field: the scaled value without the P-bit, rounded and held to `bits` bits. */
    int quantized = (scaled - (int)p + 1) / 2;

    if (quantized < 0) {
        quantized = 0;
    }

    if (quantized > (int)((1u << bits) - 1u)) {
        quantized = (int)((1u << bits) - 1u);
    }

    return (unsigned)quantized;
}

/** The 8-bit value that a raw `bits`-bit field and its P-bit stand for. */
static uint8_t endpoint_of(unsigned quantized, unsigned p, unsigned bits) {
    return unquantize((quantized << 1) | p, bits + 1u);
}

/**
 * Fits one subset of a partition: a bounding box over the pixels that belong to it, then least
 * squares over the indices that box produced. Fitting each subset separately is the whole reason
 * a partitioned mode beats a single line through the block. The best fit found goes to `fit`.
 */
static void fit_subset(
    const uint8_t        pixels[16][4],
    const uint8_t        membership[16],
    uint8_t              subset,
    const subset_layout *layout,
    unsigned             refits,
    subset_fit          *fit) {
    int      low[4]  = { 255, 255, 255, 255 };
    int      high[4] = { 0, 0, 0, 0 };
    unsigned count   = 0;

    fit->error = ~(uint64_t)0;

    /* The bounding box of the subset's pixels; a subset with no pixels gets zero endpoints. */
    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        if (membership[pixel] != subset) {
            continue;
        }

        ++count;

        for (unsigned channel = 0; channel < layout->channels; ++channel) {
            const int sample = pixels[pixel][channel];

            if (sample < low[channel]) {
                low[channel] = sample;
            }

            if (sample > high[channel]) {
                high[channel] = sample;
            }
        }
    }

    if (count == 0) {
        for (unsigned channel = 0; channel < 4; ++channel) {
            low[channel]  = 0;
            high[channel] = 0;
        }
    }

    /*
     * One pass for the box and one for each refit. A pass tries each pair of P-bits and keeps the
     * best fit so far; then the box is refitted to that fit's indices for the next pass.
     */
    for (unsigned pass = 0; pass <= refits; ++pass) {
        for (unsigned first = 0; first < 2; ++first) {
            for (unsigned second = 0; second < 2; ++second) {
                unsigned quantized[2][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
                uint8_t  endpoints[2][4];
                uint8_t  indices[16];
                uint64_t error = 0;

                /* A shared P-bit is one bit for the subset, so only the matching pair exists. */
                if (layout->shared_p && first != second) {
                    continue;
                }

                /* The endpoints as the raw fields of the block, and the values they stand for. */
                for (unsigned channel = 0; channel < layout->channels; ++channel) {
                    quantized[0][channel] = quantize_at(low[channel], first, layout->bits);
                    quantized[1][channel] = quantize_at(high[channel], second, layout->bits);
                    endpoints[0][channel] = endpoint_of(quantized[0][channel], first, layout->bits);
                    endpoints[1][channel] = endpoint_of(quantized[1][channel], second, layout->bits);
                }

                /* Each pixel of the subset takes the index whose colour comes closest to it. */
                memset(indices, 0, sizeof indices);

                for (unsigned pixel = 0; pixel < 16; ++pixel) {
                    uint32_t best = 0xffffffffu;

                    if (membership[pixel] != subset) {
                        continue;
                    }

                    for (unsigned entry = 0; entry < layout->weight_count; ++entry) {
                        uint32_t candidate = 0;

                        for (unsigned channel = 0; channel < layout->channels; ++channel) {
                            const int difference = (int)pixels[pixel][channel] -
                                (int)interpolate(endpoints[0][channel], endpoints[1][channel],
                                    layout->weights[entry]);

                            candidate += (uint32_t)(difference * difference);
                        }

                        if (candidate < best) {
                            best           = candidate;
                            indices[pixel] = (uint8_t)entry;
                        }
                    }

                    error += best;
                }

                if (error < fit->error) {
                    fit->error = error;
                    fit->p[0]  = first;
                    fit->p[1]  = second;
                    memcpy(fit->quantized, quantized, sizeof fit->quantized);
                    memcpy(fit->indices, indices, sizeof fit->indices);
                }
            }
        }

        /* The refit: least squares as in refit_endpoints, over this subset's pixels alone. */
        if (pass < refits && count > 0) {
            double a = 0;
            double b = 0;
            double c = 0;
            double determinant;

            for (unsigned pixel = 0; pixel < 16; ++pixel) {
                double weight;

                if (membership[pixel] != subset) {
                    continue;
                }

                weight  = (double)layout->weights[fit->indices[pixel]] / 64.0;
                a      += (1.0 - weight) * (1.0 - weight);
                b      += (1.0 - weight) * weight;
                c      += weight * weight;
            }

            determinant = a * c - b * b;

            if (determinant <= 1e-9 && determinant >= -1e-9) {
                continue;
            }

            for (unsigned channel = 0; channel < layout->channels; ++channel) {
                double low_sum  = 0;
                double high_sum = 0;

                for (unsigned pixel = 0; pixel < 16; ++pixel) {
                    double weight;

                    if (membership[pixel] != subset) {
                        continue;
                    }

                    weight    = (double)layout->weights[fit->indices[pixel]] / 64.0;
                    low_sum  += (1.0 - weight) * (double)pixels[pixel][channel];
                    high_sum += weight * (double)pixels[pixel][channel];
                }

                low[channel]  = clamp_byte((int)((low_sum * c - high_sum * b) / determinant + 0.5));
                high[channel] = clamp_byte((int)((high_sum * a - low_sum * b) / determinant + 0.5));
            }
        }
    }
}

/**
 * How badly one partition splits this block, cheaply: the variance left inside each subset. Fitting
 * all sixty-four partitions properly would cost more than the rest of a conversion put together, so
 * the partitions are ranked by this first and only the best few are fitted. The lower the score,
 * the better the split.
 */
static uint64_t partition_score(const uint8_t pixels[16][4], const uint8_t membership[16], unsigned channels) {
    uint32_t sums[2][4]    = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
    uint32_t squares[2][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
    uint32_t counts[2]     = { 0, 0 };
    uint64_t total         = 0;

    /* Each subset's pixel count, and its sum and sum of squares channel by channel. */
    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        const unsigned subset = membership[pixel];

        ++counts[subset];

        for (unsigned channel = 0; channel < channels; ++channel) {
            const uint32_t sample = pixels[pixel][channel];

            sums[subset][channel]    += sample;
            squares[subset][channel] += sample * sample;
        }
    }

    /* The squared distances from each subset's mean, summed: the squares less sum^2 / count. */
    for (unsigned subset = 0; subset < 2; ++subset) {
        if (counts[subset] == 0) {
            continue;
        }

        for (unsigned channel = 0; channel < channels; ++channel) {
            total += squares[subset][channel] - (uint64_t)sums[subset][channel] * sums[subset][channel] / counts[subset];
        }
    }

    return total;
}

/**
 * Writes a fitted two-subset block. The anchor of each subset carries one bit fewer, so a subset
 * whose anchor wants a high index has its endpoints swapped and its indices mirrored instead. The
 * swap is made in `fits` itself.
 */
static void write_two_subset(
    const subset_layout *layout,
    uint32_t             partition,
    subset_fit           fits[2],
    uint8_t              block[16]) {
    const uint8_t *membership = partitions_two[partition];
    const unsigned anchor     = anchor_second_of_two[partition];
    const unsigned mirror     = layout->weight_count - 1u;
    unsigned       at         = 0;
    uint8_t        indices[16];

    for (unsigned subset = 0; subset < 2; ++subset) {
        const unsigned subset_anchor = subset == 0 ? 0u : anchor;

        if (fits[subset].indices[subset_anchor] > mirror / 2u) {
            unsigned       swapped[4];
            const unsigned kept = fits[subset].p[0];

            memcpy(swapped, fits[subset].quantized[0], sizeof swapped);
            memcpy(fits[subset].quantized[0], fits[subset].quantized[1], sizeof swapped);
            memcpy(fits[subset].quantized[1], swapped, sizeof swapped);
            fits[subset].p[0] = fits[subset].p[1];
            fits[subset].p[1] = kept;

            for (unsigned pixel = 0; pixel < 16; ++pixel) {
                if (membership[pixel] == subset) {
                    fits[subset].indices[pixel] = (uint8_t)(mirror - fits[subset].indices[pixel]);
                }
            }
        }
    }

    /* Every pixel's index, from the fit of its own subset. */
    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        indices[pixel] = fits[membership[pixel]].indices[pixel];
    }

    /*
     * The block, zeroed first: the mode, the partition, the endpoint values channel by channel,
     * the P-bits, then the indices, each anchor's one bit shorter.
     */
    memset(block, 0, 16);

    /* A mode is named in unary: as many zero bits as its number, then a one. */
    put_bits(block, &at, 1u << layout->mode, layout->mode + 1u);
    put_bits(block, &at, partition, 6);

    for (unsigned channel = 0; channel < layout->channels; ++channel) {
        for (unsigned subset = 0; subset < 2; ++subset) {
            put_bits(block, &at, fits[subset].quantized[0][channel], layout->bits);
            put_bits(block, &at, fits[subset].quantized[1][channel], layout->bits);
        }
    }

    for (unsigned subset = 0; subset < 2; ++subset) {
        put_bits(block, &at, fits[subset].p[0], 1);

        if (!layout->shared_p) {
            put_bits(block, &at, fits[subset].p[1], 1);
        }
    }

    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        const unsigned bits = layout->index_bits - (pixel == 0 || pixel == anchor ? 1u : 0u);

        put_bits(block, &at, indices[pixel], bits);
    }
}

/**
 * Tries one two-subset mode against the error a single line already achieved. Quality buys the
 * search: at quality zero no partition is fitted at all, and at one the best four of the
 * sixty-four are fitted properly and kept if any of them wins. Returns 1 when one wins, with the
 * block written and `*error` lowered to its error; 0 when none does, leaving `block` alone.
 */
static int better_two_subset_block(
    const uint8_t        pixels[16][4],
    const subset_layout *layout,
    unsigned             refits,
    uint64_t            *error,
    uint8_t              block[16]) {
    /* The best-scored partitions so far, the lowest score first, and how many are wanted. */
    uint64_t       ranked[4]     = { ~(uint64_t)0, ~(uint64_t)0, ~(uint64_t)0, ~(uint64_t)0 };
    uint32_t       candidates[4] = { 0, 0, 0, 0 };
    const unsigned wanted        = refits > 4u ? 4u : refits;

    /* The winning partition and its fits, once one beats `*error`. */
    subset_fit best_fits[2];
    uint32_t   best_partition = 0;
    int        found          = 0;

    if (wanted == 0) {
        return 0;
    }

    /* Every partition scored, and slotted into the ranking when it beats one of those kept. */
    for (uint32_t partition = 0; partition < 64; ++partition) {
        const uint64_t score = partition_score(pixels, partitions_two[partition], layout->channels);

        for (unsigned slot = 0; slot < wanted; ++slot) {
            if (score < ranked[slot]) {
                for (unsigned move = wanted - 1u; move > slot; --move) {
                    ranked[move]     = ranked[move - 1u];
                    candidates[move] = candidates[move - 1u];
                }

                ranked[slot]     = score;
                candidates[slot] = partition;
                break;
            }
        }
    }

    /* The ranked partitions fitted properly, both subsets each, against the error so far. */
    for (unsigned slot = 0; slot < wanted; ++slot) {
        subset_fit fits[2];
        uint64_t   candidate;

        if (ranked[slot] == ~(uint64_t)0) {
            break;
        }

        fit_subset(pixels, partitions_two[candidates[slot]], 0, layout, refits, &fits[0]);
        fit_subset(pixels, partitions_two[candidates[slot]], 1, layout, refits, &fits[1]);
        candidate = fits[0].error + fits[1].error;

        if (candidate < *error) {
            *error         = candidate;
            best_partition = candidates[slot];
            memcpy(best_fits, fits, sizeof best_fits);
            found = 1;
        }
    }

    if (found) {
        write_two_subset(layout, best_partition, best_fits, block);
    }

    return found;
}

/**
 * Encodes 16 BGRA pixels as one block. Mode 6 is fitted first: one line through the block, refitted
 * `refits` times. Then mode 1 for an opaque block, or mode 7 for one with alpha, is tried against
 * it, and the block is written in whichever fits better.
 */
void edds_bc7_encode_block(const uint8_t bgra[64], unsigned refits, uint8_t block[16]) {
    /* The pixels as RGBA, and the endpoints the next pass starts from. */
    uint8_t pixels[16][4];
    int     low[4];
    int     high[4];

    /* The best mode 6 fit so far: its endpoints, indices, P-bits and error. */
    uint8_t  best_low[4]      = { 0, 0, 0, 0 };
    uint8_t  best_high[4]     = { 0, 0, 0, 0 };
    uint8_t  best_indices[16] = { 0 };
    unsigned best_p[2]        = { 0, 0 };
    uint64_t best_error       = ~(uint64_t)0;

    /* Where the next bit of the block is written. */
    unsigned at = 0;

    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        pixels[pixel][0] = bgra[pixel * 4u + 2u];
        pixels[pixel][1] = bgra[pixel * 4u + 1u];
        pixels[pixel][2] = bgra[pixel * 4u];
        pixels[pixel][3] = bgra[pixel * 4u + 3u];
    }

    /* The first fit, the bounding box: the lowest and the highest value of each channel. */
    for (unsigned channel = 0; channel < 4; ++channel) {
        low[channel]  = 255;
        high[channel] = 0;

        for (unsigned pixel = 0; pixel < 16; ++pixel) {
            const int sample = pixels[pixel][channel];

            if (sample < low[channel]) {
                low[channel] = sample;
            }

            if (sample > high[channel]) {
                high[channel] = sample;
            }
        }
    }

    /*
     * One pass for the box and one for each refit. A pass tries the four pairs of P-bits and keeps
     * the best fit so far; then the endpoints are refitted to that fit's indices for the next pass.
     */
    for (unsigned pass = 0; pass <= refits; ++pass) {
        for (unsigned p0 = 0; p0 < 2; ++p0) {
            for (unsigned p1 = 0; p1 < 2; ++p1) {
                uint8_t  candidate_low[4];
                uint8_t  candidate_high[4];
                uint8_t  palette[16][4];
                uint8_t  indices[16];
                uint64_t error;

                /* Each endpoint value as seven bits, with the P-bit below them. */
                for (unsigned channel = 0; channel < 4; ++channel) {
                    const uint8_t seven_low  = quantize_with_p(low[channel], p0);
                    const uint8_t seven_high = quantize_with_p(high[channel], p1);

                    candidate_low[channel]  = (uint8_t)((seven_low << 1) | p0);
                    candidate_high[channel] = (uint8_t)((seven_high << 1) | p1);
                }

                mode_six_palette(candidate_low, candidate_high, palette);
                error = assign_indices(pixels, palette, indices);

                if (error < best_error) {
                    best_error = error;
                    memcpy(best_low, candidate_low, sizeof best_low);
                    memcpy(best_high, candidate_high, sizeof best_high);
                    memcpy(best_indices, indices, sizeof best_indices);
                    best_p[0] = p0;
                    best_p[1] = p1;
                }
            }
        }

        if (pass < refits) {
            refit_endpoints(pixels, best_indices, low, high);

            for (unsigned channel = 0; channel < 4; ++channel) {
                low[channel]  = clamp_byte(low[channel]);
                high[channel] = clamp_byte(high[channel]);
            }
        }
    }

    /*
     * A block whose colours do not sit on one line is exactly what the partitioned modes are for.
     * Mode 1 spends every bit it has on two colour lines and leaves alpha opaque; mode 7 partitions
     * the alpha too, at fewer bits per channel and half the index levels. So the two never compete:
     * an opaque block is mode 1's, because mode 7 could only describe the same colours with less of
     * everything, and a block with alpha in it is mode 7's, because mode 1 cannot carry its alpha
     * at all. Whichever applies is kept only if it beats the single line mode 6 found; quality buys
     * the search behind it.
     */
    {
        uint64_t contested = best_error;
        int      opaque    = 1;

        for (unsigned pixel = 0; pixel < 16 && opaque; ++pixel) {
            opaque = pixels[pixel][3] == 255u;
        }

        if (better_two_subset_block(pixels, opaque ? &layout_mode_one : &layout_mode_seven, refits, &contested, block)) {
            return;
        }
    }

    /*
     * The first index carries one bit fewer, so the endpoints are ordered to leave its top bit 0.
     */
    if (best_indices[0] >= 8) {
        uint8_t        swapped[4];
        const unsigned kept = best_p[0];

        memcpy(swapped, best_low, sizeof swapped);
        memcpy(best_low, best_high, sizeof best_low);
        memcpy(best_high, swapped, sizeof best_high);
        best_p[0] = best_p[1];
        best_p[1] = kept;

        for (unsigned pixel = 0; pixel < 16; ++pixel) {
            best_indices[pixel] = (uint8_t)(15u - best_indices[pixel]);
        }
    }

    /*
     * The mode 6 block, zeroed first: the mode, each channel's two endpoints as seven bits, the
     * two P-bits, then the indices, the first one 3 bits long and the rest 4.
     */
    memset(block, 0, 16);
    put_bits(block, &at, 1u << 6, MODE_SIX_BITS);

    for (unsigned channel = 0; channel < 4; ++channel) {
        put_bits(block, &at, (uint32_t)(best_low[channel] >> 1), 7);
        put_bits(block, &at, (uint32_t)(best_high[channel] >> 1), 7);
    }

    put_bits(block, &at, best_p[0], 1);
    put_bits(block, &at, best_p[1], 1);

    for (unsigned pixel = 0; pixel < 16; ++pixel) {
        put_bits(block, &at, best_indices[pixel], pixel == 0 ? 3u : 4u);
    }
}
