#include "bc6.h"
#include <float.h>
#include <math.h>
#include <string.h>
#include "bc6_tables.h"

static const uint8_t weights4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };
static const uint8_t weights3[8]  = { 0, 9, 18, 27, 37, 46, 55, 64 };

/** Read a little-endian field entirely inside the 128-bit block. */
static uint32_t bits(const uint8_t *block, uint32_t start, uint32_t count) {
    uint32_t value = 0;

    for (uint32_t i = 0; i < count; ++i) {
        value |= ((block[(start + i) / 8] >> ((start + i) % 8)) & 1u) << i;
    }

    return value;
}

/** Set a field in an initially zeroed block; fields do not overlap. */
static void put(uint8_t *block, uint32_t start, uint32_t count, uint32_t value) {
    for (uint32_t i = 0; i < count; ++i) {
        block[(start + i) / 8] |= (uint8_t)(((value >> i) & 1u) << ((start + i) % 8));
    }
}

/** Expand an unsigned endpoint to the BC6H interpolation domain, preserving its extrema. */
static uint32_t unquantize(uint32_t value, uint32_t precision) {
    if (precision >= 15) {
        return value;
    }

    if (value == 0) {
        return 0;
    }

    if (value == (1u << precision) - 1) {
        return 65535;
    }

    return ((value << 16) + 32768) >> precision;
}

/** Convert a finite, unsigned half bit pattern to float, including subnormal values. */
static float half_float(uint32_t half) {
    uint32_t exponent = half >> 10, fraction = half & 1023;

    return exponent == 0 ? ldexpf((float)fraction, -24) : ldexpf((float)(1024 + fraction), (int)exponent - 25);
}

/** Interpolate in the BC6H endpoint domain and apply its final half-float scaling. */
static float sample(uint32_t a, uint32_t b, uint32_t weight) {
    return half_float((((a * (64 - weight) + b * weight + 32) >> 6) * 31) >> 6);
}

/** Decode any legal mode to linear RGBA; reserved modes return opaque black. */
void edds_bc6_decode(const uint8_t block[16], float rgba[64]) {
    /* Mode codes and endpoint precision defined by the BC6H bit layout. */
    static const uint8_t modes[14]     = { 0, 1, 2, 6, 10, 14, 18, 22, 26, 30, 3, 7, 11, 15 };
    static const uint8_t precision[14] = { 10, 7, 11, 11, 11, 9, 8, 8, 8, 6, 10, 11, 12, 16 };

    static const uint8_t delta[14][3] = {
        { 5, 5, 5 }, { 6, 6, 6 }, { 5, 4, 4 }, { 4, 5, 4 }, { 4, 4, 5 }, { 5, 5, 5 }, { 6, 5, 5 },
        { 5, 6, 5 }, { 5, 5, 6 }, { 6, 6, 6 }, { 10, 10, 10 }, { 9, 9, 9 }, { 8, 8, 8 }, { 4, 4, 4 }
    };

    /* Selected mode and its unpacked endpoint values. */
    uint32_t code = bits(block, 0, 2), mode = 14, endpoints[12] = { 0 };

    if (code > 1) {
        code = bits(block, 0, 5);
    }

    for (uint32_t i = 0; i < 14; ++i) {
        if (code == modes[i]) {
            mode = i;
            break;
        }
    }

    /* Reserved modes have the GPU-mandated black RGB, opaque alpha. */
    for (uint32_t i = 0; i < 16; ++i) {
        rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 0;
        rgba[i * 4 + 3]                                 = 1;
    }

    if (mode == 14) {
        return;
    }

    const uint32_t two = mode < 10, count = two ? 12 : 6, base = precision[mode];

    for (uint32_t bit = 0; bit < (two ? 77u : 65u); ++bit) {
        const uint32_t target = endpoint_bits[mode][bit];

        if (target != 255) {
            endpoints[target / 16] |= bits(block, bit, 1) << (target % 16);
        }
    }

    if (mode != 9 && mode != 10) {
        for (uint32_t i = 3; i < count; ++i) {
            uint32_t n = delta[mode][i % 3], v = endpoints[i];
            int32_t  d = (int32_t)v;

            if (v & (1u << (n - 1))) {
                d -= (int32_t)(1u << n);
            }

            endpoints[i] = (uint32_t)((int32_t)endpoints[i % 3] + d) & ((1u << base) - 1);
        }
    }

    for (uint32_t i = 0; i < count; ++i) {
        endpoints[i] = unquantize(endpoints[i], base);
    }

    const uint32_t partition = two ? bits(block, 77, 5) : 0;
    uint32_t       at        = two ? 82 : 65;

    for (uint32_t pixel = 0; pixel < 16; ++pixel) {
        uint32_t subset = two ? ((partitions[partition] >> pixel) & 1) : 0;
        uint32_t anchor = pixel == 0 || (two && pixel == anchors[partition]);
        uint32_t n = (two ? 3u : 4u) - anchor, index = bits(block, at, n);
        uint32_t w = two ? weights3[index] : weights4[index];

        at += n;

        for (uint32_t c = 0; c < 3; ++c) {
            rgba[pixel * 4 + c] = sample(endpoints[subset * 6 + c], endpoints[subset * 6 + 3 + c], w);
        }
    }
}

/* Minimise relative squared RGB error using decoded palette values, not byte or half-code error. */
static double evaluate(const float *rgba, const uint32_t endpoint[6], uint8_t indices[16]) {
    float  palette[16][3];
    double error = 0;

    for (uint32_t i = 0; i < 16; ++i) {
        for (uint32_t c = 0; c < 3; ++c) {
            palette[i][c] = sample(unquantize(endpoint[c], 10), unquantize(endpoint[c + 3], 10), weights4[i]);
        }
    }

    for (uint32_t i = 0; i < 16; ++i) {
        double best = DBL_MAX;

        for (uint32_t j = 0; j < 16; ++j) {
            double distance = 0;

            for (uint32_t c = 0; c < 3; ++c) {
                double value = rgba[i * 4 + c], d = (palette[j][c] - value) / (1 + value);

                distance += d * d;
            }

            if (distance < best) {
                best       = distance;
                indices[i] = (uint8_t)j;
            }
        }

        error += best;
    }

    return error;
}

/* Nearest decoded mode-11 endpoint. Binary search avoids dependence on host half intrinsics. */
static uint32_t quantize(float value) {
    uint32_t lo = 0, hi = 1023;

    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;

        if (sample(unquantize(mid, 10), 0, 0) < value) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    if (lo > 0 && value - sample(unquantize(lo - 1, 10), 0, 0) < sample(unquantize(lo, 10), 0, 0) - value) {
        --lo;
    }

    return lo;
}

/** Fit a mode-11 block to finite nonnegative RGB; quality bounds deterministic search effort. */
void edds_bc6_encode(const float rgba[64], uint32_t quality, uint8_t block[16]) {
    uint32_t endpoint[6];
    uint8_t  indices[16];

    for (uint32_t c = 0; c < 3; ++c) {
        float lo = rgba[c], hi = rgba[c];

        for (uint32_t i = 1; i < 16; ++i) {
            lo = fminf(lo, rgba[i * 4 + c]);
            hi = fmaxf(hi, rgba[i * 4 + c]);
        }

        endpoint[c]     = quantize(lo);
        endpoint[c + 3] = quantize(hi);
    }

    double best = evaluate(rgba, endpoint, indices);

    /* Colour ramps need endpoints along their colour direction, not opposite RGB box corners. */
    for (uint32_t axis = 0; axis < quality * 3 / 1000; ++axis) {
        uint32_t lo = 0, hi = 0, candidate[6];

        for (uint32_t i = 1; i < 16; ++i) {
            if (rgba[i * 4 + axis] < rgba[lo * 4 + axis]) {
                lo = i;
            }

            if (rgba[i * 4 + axis] > rgba[hi * 4 + axis]) {
                hi = i;
            }
        }

        for (uint32_t c = 0; c < 3; ++c) {
            candidate[c]     = quantize(rgba[lo * 4 + c]);
            candidate[c + 3] = quantize(rgba[hi * 4 + c]);
        }

        double error = evaluate(rgba, candidate, indices);

        if (error < best) {
            best = error;
            memcpy(endpoint, candidate, sizeof endpoint);
        }
    }

    const uint32_t rounds = quality * 8 / 1000;

    for (uint32_t round = 0; round < rounds; ++round) {
        int changed = 0;

        for (uint32_t c = 0; c < 6; ++c) {
            uint32_t original = endpoint[c], winner = original;

            for (int step = -1; step <= 1; step += 2) {
                int candidate = (int)original + step;

                if (candidate < 0 || candidate > 1023) {
                    continue;
                }

                endpoint[c]  = (uint32_t)candidate;
                double error = evaluate(rgba, endpoint, indices);

                if (error < best) {
                    best    = error;
                    winner  = (uint32_t)candidate;
                    changed = 1;
                }
            }

            endpoint[c] = winner;
        }

        if (!changed) {
            break;
        }
    }

    (void)evaluate(rgba, endpoint, indices);

    if (indices[0] >= 8) {
        for (uint32_t c = 0; c < 3; ++c) {
            uint32_t t = endpoint[c];

            endpoint[c]     = endpoint[c + 3];
            endpoint[c + 3] = t;
        }

        for (uint32_t i = 0; i < 16; ++i) {
            indices[i] = (uint8_t)(15 - indices[i]);
        }
    }

    memset(block, 0, 16);
    put(block, 0, 5, 3);

    for (uint32_t c = 0; c < 6; ++c) {
        put(block, 5 + c * 10, 10, endpoint[c]);
    }

    uint32_t at = 65;

    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t n = i == 0 ? 3 : 4;

        put(block, at, n, indices[i]);
        at += n;
    }
}
