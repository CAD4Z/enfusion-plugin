/* Big-endian reads from a TrueType file, and the bounds check that guards every one of them. */
#ifndef FONT_BYTES_H
#define FONT_BYTES_H

#include <stdint.h>

/** A four-character tag as the 32-bit number it is stored as: `a` in the high byte. */
#define TAG(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/** The unsigned 16-bit number stored big-endian at `at`. */
static inline uint16_t u16(const uint8_t *at) {
    return (uint16_t)(((unsigned)at[0] << 8) | at[1]);
}

/** The signed 16-bit number stored big-endian at `at`. */
static inline int16_t s16(const uint8_t *at) {
    return (int16_t)u16(at);
}

/** The unsigned 32-bit number stored big-endian at `at`. */
static inline uint32_t u32(const uint8_t *at) {
    return ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16) | ((uint32_t)at[2] << 8) | at[3];
}

/** Whether `length` bytes from `offset` lie inside `size` bytes, without overflowing. */
static inline int inside(uint64_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= size - offset;
}

#endif
