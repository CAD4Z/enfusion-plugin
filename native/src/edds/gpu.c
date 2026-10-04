/*
 * The GPU formats a conversion can produce, and the decode that shows what one of them actually
 * holds. Every block codec here is symmetric with the decoder beside it, and the independent test
 * reader implements its own: this file is not allowed to be the only thing that agrees with itself.
 */
#include "gpu.h"
#include "bc7.h"

#include <string.h>

enum { BLOCK_PIXELS = 16 };

uint32_t edds_gpu_block_bytes(edds_pixel_format format) {
    switch (format) {
        case EDDS_PIXEL_DXT1: return 8;
        case EDDS_PIXEL_BC4: return 8;
        case EDDS_PIXEL_DXT5: return 16;
        case EDDS_PIXEL_BC5: return 16;
        case EDDS_PIXEL_BC7: return 16;
        default: return 0;
    }
}

uint32_t edds_gpu_pixel_bytes(edds_pixel_format format) {
    switch (format) {
        case EDDS_PIXEL_BGRA8: return 4;
        case EDDS_PIXEL_BGRX8: return 4;
        case EDDS_PIXEL_R8: return 1;
        case EDDS_PIXEL_RG8: return 2;
        default: return 0;
    }
}

uint32_t edds_gpu_mip_bytes(edds_pixel_format format, uint32_t width, uint32_t height) {
    const uint32_t block = edds_gpu_block_bytes(format);
    const uint32_t pixel = edds_gpu_pixel_bytes(format);
    uint64_t bytes;
    if (width == 0 || height == 0 || width > EDDS_MAX_DIMENSION || height > EDDS_MAX_DIMENSION) {
        return 0;
    }
    if (block != 0) {
        bytes = (uint64_t)((width + 3u) / 4u) * ((height + 3u) / 4u) * block;
    } else if (pixel != 0) {
        bytes = (uint64_t)width * height * pixel;
    } else {
        return 0;
    }
    return bytes == 0 || bytes > UINT32_MAX ? 0u : (uint32_t)bytes;
}

const char *edds_pixel_format_name(edds_pixel_format format) {
    switch (format) {
        case EDDS_PIXEL_BGRA8: return "BGRA8";
        case EDDS_PIXEL_BGRX8: return "BGRX8";
        case EDDS_PIXEL_R8: return "R8";
        case EDDS_PIXEL_RG8: return "RG8";
        case EDDS_PIXEL_DXT1: return "DXT1";
        case EDDS_PIXEL_DXT5: return "DXT5";
        case EDDS_PIXEL_BC4: return "BC4";
        case EDDS_PIXEL_BC5: return "BC5";
        case EDDS_PIXEL_BC7: return "BC7";
        default: return "UNKNOWN";
    }
}

/**
 * The channels a decode of this format carries, which is what the file holds rather than what the
 * source image had: a BC4 texture made from a colour photograph reports R, not RGB.
 */
const char *edds_pixel_format_channels(edds_pixel_format format) {
    switch (format) {
        case EDDS_PIXEL_BGRA8: return "RGBA";
        case EDDS_PIXEL_BGRX8: return "RGB";
        case EDDS_PIXEL_R8: return "R";
        case EDDS_PIXEL_RG8: return "RG";
        case EDDS_PIXEL_DXT1: return "RGB";
        case EDDS_PIXEL_DXT5: return "RGBA";
        case EDDS_PIXEL_BC4: return "R";
        case EDDS_PIXEL_BC5: return "RG";
        case EDDS_PIXEL_BC7: return "RGBA";
        default: return "UNKNOWN";
    }
}

static uint8_t clamp_byte(int value) {
    return value < 0 ? 0u : (value > 255 ? 255u : (uint8_t)value);
}

static uint16_t u16le(const uint8_t *at) {
    return (uint16_t)((uint16_t)at[0] | ((uint16_t)at[1] << 8));
}

static void put_u16le(uint8_t *at, uint16_t value) {
    at[0] = (uint8_t)(value & 0xffu);
    at[1] = (uint8_t)(value >> 8);
}

/* ------------------------------- BC1 colour blocks ------------------------------- */

static uint16_t to_565(const int rgb[3]) {
    const uint32_t red = (uint32_t)((rgb[0] * 31 + 127) / 255);
    const uint32_t green = (uint32_t)((rgb[1] * 63 + 127) / 255);
    const uint32_t blue = (uint32_t)((rgb[2] * 31 + 127) / 255);
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

static void from_565(uint16_t value, uint8_t rgb[3]) {
    const uint8_t red = (uint8_t)((value >> 11) & 31u);
    const uint8_t green = (uint8_t)((value >> 5) & 63u);
    const uint8_t blue = (uint8_t)(value & 31u);
    rgb[0] = (uint8_t)((red << 3) | (red >> 2));
    rgb[1] = (uint8_t)((green << 2) | (green >> 4));
    rgb[2] = (uint8_t)((blue << 3) | (blue >> 2));
}

static uint8_t mix(int low, int high, int low_parts, int high_parts, int total) {
    return (uint8_t)((low * low_parts + high * high_parts + total / 2) / total);
}

/**
 * The four colours of a block. Standing alone as BC1, the order of the endpoints chooses the
 * layout: `c0 > c1` is four colours, and otherwise the fourth entry is a transparent black rather
 * than a colour. Inside BC3 the same eight bytes are always four colours, because the alpha block
 * beside them already carries the alpha and the endpoint order means nothing — so a DXT5 written
 * with `c0 <= c1` decodes to colours, not to holes.
 */
static void bc1_palette(
    uint16_t first,
    uint16_t second,
    int always_four_colours,
    uint8_t palette[4][4]) {
    const int four_colours = always_four_colours || first > second;
    uint8_t low[3];
    uint8_t high[3];
    from_565(first, low);
    from_565(second, high);
    for (unsigned channel = 0; channel < 3; ++channel) {
        palette[0][channel] = low[channel];
        palette[1][channel] = high[channel];
        if (four_colours) {
            palette[2][channel] = mix(low[channel], high[channel], 2, 1, 3);
            palette[3][channel] = mix(low[channel], high[channel], 1, 2, 3);
        } else {
            palette[2][channel] = mix(low[channel], high[channel], 1, 1, 2);
            palette[3][channel] = 0;
        }
    }
    palette[0][3] = 255u;
    palette[1][3] = 255u;
    palette[2][3] = 255u;
    palette[3][3] = four_colours ? 255u : 0u;
}

static uint32_t bc1_indices(
    const uint8_t pixels[BLOCK_PIXELS][4],
    const uint8_t palette[4][4],
    uint8_t indices[BLOCK_PIXELS]) {
    uint32_t total = 0;
    for (unsigned pixel = 0; pixel < BLOCK_PIXELS; ++pixel) {
        uint32_t best = 0xffffffffu;
        uint8_t chosen = 0;
        for (unsigned entry = 0; entry < 4; ++entry) {
            uint32_t error = 0;
            for (unsigned channel = 0; channel < 3; ++channel) {
                const int difference = (int)pixels[pixel][channel] - (int)palette[entry][channel];
                error += (uint32_t)(difference * difference);
            }
            if (error < best) {
                best = error;
                chosen = (uint8_t)entry;
            }
        }
        indices[pixel] = chosen;
        total += best;
    }
    return total;
}

/** Least squares over the indices a fit produced; the whole of what quality buys a colour block. */
static void bc1_refit(
    const uint8_t pixels[BLOCK_PIXELS][4],
    const uint8_t indices[BLOCK_PIXELS],
    int low[3],
    int high[3]) {
    static const double share[4] = { 0.0, 1.0, 1.0 / 3.0, 2.0 / 3.0 };
    double a = 0;
    double b = 0;
    double c = 0;
    double determinant;
    for (unsigned pixel = 0; pixel < BLOCK_PIXELS; ++pixel) {
        const double weight = share[indices[pixel]];
        a += (1.0 - weight) * (1.0 - weight);
        b += (1.0 - weight) * weight;
        c += weight * weight;
    }
    determinant = a * c - b * b;
    if (determinant > -1e-9 && determinant < 1e-9) {
        return;
    }
    for (unsigned channel = 0; channel < 3; ++channel) {
        double low_sum = 0;
        double high_sum = 0;
        for (unsigned pixel = 0; pixel < BLOCK_PIXELS; ++pixel) {
            const double weight = share[indices[pixel]];
            const double sample = (double)pixels[pixel][channel];
            low_sum += (1.0 - weight) * sample;
            high_sum += weight * sample;
        }
        low[channel] = clamp_byte((int)((low_sum * c - high_sum * b) / determinant + 0.5));
        high[channel] = clamp_byte((int)((high_sum * a - low_sum * b) / determinant + 0.5));
    }
}

static void bc1_encode_block(
    const uint8_t pixels[BLOCK_PIXELS][4],
    unsigned refits,
    uint8_t block[8]) {
    int low[3];
    int high[3];
    uint16_t best_first = 0;
    uint16_t best_second = 0;
    uint8_t best_indices[BLOCK_PIXELS] = { 0 };
    uint32_t best_error = 0xffffffffu;

    for (unsigned channel = 0; channel < 3; ++channel) {
        low[channel] = 255;
        high[channel] = 0;
        for (unsigned pixel = 0; pixel < BLOCK_PIXELS; ++pixel) {
            const int sample = pixels[pixel][channel];
            if (sample < low[channel]) {
                low[channel] = sample;
            }
            if (sample > high[channel]) {
                high[channel] = sample;
            }
        }
    }

    for (unsigned pass = 0; pass <= refits; ++pass) {
        uint16_t first = to_565(high);
        uint16_t second = to_565(low);
        uint8_t palette[4][4];
        uint8_t indices[BLOCK_PIXELS];
        uint32_t error;
        if (first < second) {
            const uint16_t kept = first;
            first = second;
            second = kept;
        }
        bc1_palette(first, second, 0, palette);
        if (first == second) {
            /*
             * Equal endpoints are the three-colour layout, and its fourth entry is a transparent
             * black rather than a colour. Every entry that is a colour is the same one, so the
             * block is that colour everywhere and no index may reach the fourth.
             */
            memset(indices, 0, sizeof indices);
            error = 0;
            for (unsigned pixel = 0; pixel < BLOCK_PIXELS; ++pixel) {
                for (unsigned channel = 0; channel < 3; ++channel) {
                    const int difference = (int)pixels[pixel][channel] - (int)palette[0][channel];
                    error += (uint32_t)(difference * difference);
                }
            }
        } else {
            error = bc1_indices(pixels, palette, indices);
        }
        if (error < best_error) {
            best_error = error;
            best_first = first;
            best_second = second;
            memcpy(best_indices, indices, sizeof best_indices);
        }
        if (pass < refits) {
            bc1_refit(pixels, best_indices, low, high);
        }
    }

    put_u16le(block, best_first);
    put_u16le(block + 2, best_second);
    for (unsigned row = 0; row < 4; ++row) {
        block[4 + row] = (uint8_t)(best_indices[row * 4u] |
            (best_indices[row * 4u + 1u] << 2) |
            (best_indices[row * 4u + 2u] << 4) |
            (best_indices[row * 4u + 3u] << 6));
    }
}

static void bc1_decode_block(
    const uint8_t block[8],
    int always_four_colours,
    uint8_t rgba[BLOCK_PIXELS * 4]) {
    uint8_t palette[4][4];
    bc1_palette(u16le(block), u16le(block + 2), always_four_colours, palette);
    for (unsigned pixel = 0; pixel < BLOCK_PIXELS; ++pixel) {
        const unsigned index = (block[4 + pixel / 4u] >> ((pixel % 4u) * 2u)) & 3u;
        memcpy(rgba + pixel * 4u, palette[index], 4);
    }
}

/* ------------------------------- BC4 single channel ------------------------------- */

static void bc4_palette(uint8_t first, uint8_t second, uint8_t palette[8]) {
    palette[0] = first;
    palette[1] = second;
    if (first > second) {
        for (unsigned entry = 2; entry < 8; ++entry) {
            palette[entry] = mix(first, second, (int)(8u - entry), (int)(entry - 1u), 7);
        }
    } else {
        for (unsigned entry = 2; entry < 6; ++entry) {
            palette[entry] = mix(first, second, (int)(6u - entry), (int)(entry - 1u), 5);
        }
        palette[6] = 0;
        palette[7] = 255;
    }
}

static uint32_t bc4_indices(
    const uint8_t values[BLOCK_PIXELS],
    const uint8_t palette[8],
    uint8_t indices[BLOCK_PIXELS]) {
    uint32_t total = 0;
    for (unsigned pixel = 0; pixel < BLOCK_PIXELS; ++pixel) {
        uint32_t best = 0xffffffffu;
        uint8_t chosen = 0;
        for (unsigned entry = 0; entry < 8; ++entry) {
            const int difference = (int)values[pixel] - (int)palette[entry];
            const uint32_t error = (uint32_t)(difference * difference);
            if (error < best) {
                best = error;
                chosen = (uint8_t)entry;
            }
        }
        indices[pixel] = chosen;
        total += best;
    }
    return total;
}

static void bc4_write(uint8_t first, uint8_t second, const uint8_t indices[BLOCK_PIXELS], uint8_t block[8]) {
    uint64_t packed = 0;
    block[0] = first;
    block[1] = second;
    for (unsigned pixel = 0; pixel < BLOCK_PIXELS; ++pixel) {
        packed |= (uint64_t)(indices[pixel] & 7u) << (pixel * 3u);
    }
    for (unsigned byte = 0; byte < 6; ++byte) {
        block[2 + byte] = (uint8_t)((packed >> (byte * 8u)) & 0xffu);
    }
}

/**
 * The eight-value layout is the first fit; the six-value one, which spends two of its entries on
 * an exact 0 and an exact 255, is tried only when quality pays for the second search. A block that
 * really does hold both extremes plus mid tones is where the two differ.
 */
static void bc4_encode_block(
    const uint8_t values[BLOCK_PIXELS],
    unsigned refits,
    uint8_t block[8]) {
    uint8_t palette[8];
    uint8_t indices[BLOCK_PIXELS];
    uint8_t best_indices[BLOCK_PIXELS];
    uint8_t lowest = 255;
    uint8_t highest = 0;
    uint8_t best_first;
    uint8_t best_second;
    uint32_t best_error;

    for (unsigned pixel = 0; pixel < BLOCK_PIXELS; ++pixel) {
        if (values[pixel] < lowest) {
            lowest = values[pixel];
        }
        if (values[pixel] > highest) {
            highest = values[pixel];
        }
    }
    best_first = highest;
    best_second = lowest;
    bc4_palette(best_first, best_second, palette);
    best_error = bc4_indices(values, palette, best_indices);

    if (refits > 0 && best_first != best_second) {
        uint8_t inner_low = 255;
        uint8_t inner_high = 0;
        for (unsigned pixel = 0; pixel < BLOCK_PIXELS; ++pixel) {
            if (values[pixel] == 0 || values[pixel] == 255) {
                continue;
            }
            if (values[pixel] < inner_low) {
                inner_low = values[pixel];
            }
            if (values[pixel] > inner_high) {
                inner_high = values[pixel];
            }
        }
        if (inner_low <= inner_high) {
            uint32_t error;
            bc4_palette(inner_low, inner_high, palette);
            error = bc4_indices(values, palette, indices);
            if (error < best_error) {
                best_error = error;
                best_first = inner_low;
                best_second = inner_high;
                memcpy(best_indices, indices, sizeof best_indices);
            }
        }
    }

    bc4_write(best_first, best_second, best_indices, block);
}

static void bc4_decode_block(const uint8_t block[8], uint8_t values[BLOCK_PIXELS]) {
    uint8_t palette[8];
    uint64_t packed = 0;
    bc4_palette(block[0], block[1], palette);
    for (unsigned byte = 0; byte < 6; ++byte) {
        packed |= (uint64_t)block[2 + byte] << (byte * 8u);
    }
    for (unsigned pixel = 0; pixel < BLOCK_PIXELS; ++pixel) {
        values[pixel] = palette[(packed >> (pixel * 3u)) & 7u];
    }
}

/* ------------------------------- blocks and dispatch ------------------------------- */

/**
 * Reads the 4x4 block at (x, y) out of a BGRA mip. A block that hangs off the right or the bottom
 * repeats the last real column and row: the padding is never seen by a sampler, and repeating the
 * edge keeps it from pulling the endpoints of the block towards a colour the image does not have.
 */
static void gather_block(
    const uint8_t *bgra,
    uint32_t width,
    uint32_t height,
    uint32_t left,
    uint32_t top,
    uint8_t pixels[BLOCK_PIXELS][4]) {
    for (unsigned row = 0; row < 4; ++row) {
        const uint32_t y = top + row < height ? top + row : height - 1u;
        for (unsigned column = 0; column < 4; ++column) {
            const uint32_t x = left + column < width ? left + column : width - 1u;
            const size_t at = ((size_t)y * width + x) * 4u;
            uint8_t *target = pixels[row * 4u + column];
            target[0] = bgra[at + 2u];
            target[1] = bgra[at + 1u];
            target[2] = bgra[at];
            target[3] = bgra[at + 3u];
        }
    }
}

/** Workbench calls quality a ratio; here it is the number of search passes it buys. */
enum { MOST_REFITS = 4 };

static unsigned refits_of(uint32_t quality) {
    const uint32_t bounded = quality > EDDS_QUALITY_SCALE ? EDDS_QUALITY_SCALE : quality;
    return (unsigned)((bounded * MOST_REFITS) / EDDS_QUALITY_SCALE);
}

void edds_gpu_encode(
    edds_pixel_format format,
    uint32_t quality,
    const uint8_t *bgra,
    uint32_t width,
    uint32_t height,
    uint8_t *output) {
    const unsigned refits = refits_of(quality);
    const uint32_t block_bytes = edds_gpu_block_bytes(format);
    if (block_bytes == 0) {
        for (size_t pixel = 0; pixel < (size_t)width * height; ++pixel) {
            if (format == EDDS_PIXEL_R8) {
                output[pixel] = bgra[pixel * 4u + 2u];
            } else if (format == EDDS_PIXEL_RG8) {
                output[pixel * 2u] = bgra[pixel * 4u + 2u];
                output[pixel * 2u + 1u] = bgra[pixel * 4u + 1u];
            } else {
                memcpy(output + pixel * 4u, bgra + pixel * 4u, 4);
            }
        }
        return;
    }
    {
        const uint32_t columns = (width + 3u) / 4u;
        const uint32_t rows = (height + 3u) / 4u;
        for (uint32_t row = 0; row < rows; ++row) {
            for (uint32_t column = 0; column < columns; ++column) {
                uint8_t pixels[BLOCK_PIXELS][4];
                uint8_t channel[BLOCK_PIXELS];
                uint8_t *block = output + ((size_t)row * columns + column) * block_bytes;
                gather_block(bgra, width, height, column * 4u, row * 4u, pixels);
                switch (format) {
                    case EDDS_PIXEL_DXT1:
                        bc1_encode_block(pixels, refits, block);
                        break;
                    case EDDS_PIXEL_DXT5:
                        for (unsigned pixel = 0; pixel < BLOCK_PIXELS; ++pixel) {
                            channel[pixel] = pixels[pixel][3];
                        }
                        bc4_encode_block(channel, refits, block);
                        bc1_encode_block(pixels, refits, block + 8);
                        break;
                    case EDDS_PIXEL_BC4:
                        for (unsigned pixel = 0; pixel < BLOCK_PIXELS; ++pixel) {
                            channel[pixel] = pixels[pixel][0];
                        }
                        bc4_encode_block(channel, refits, block);
                        break;
                    case EDDS_PIXEL_BC5:
                        for (unsigned pixel = 0; pixel < BLOCK_PIXELS; ++pixel) {
                            channel[pixel] = pixels[pixel][0];
                        }
                        bc4_encode_block(channel, refits, block);
                        for (unsigned pixel = 0; pixel < BLOCK_PIXELS; ++pixel) {
                            channel[pixel] = pixels[pixel][1];
                        }
                        bc4_encode_block(channel, refits, block + 8);
                        break;
                    case EDDS_PIXEL_BC7: {
                        uint8_t block_bgra[BLOCK_PIXELS * 4];
                        for (unsigned pixel = 0; pixel < BLOCK_PIXELS; ++pixel) {
                            block_bgra[pixel * 4u] = pixels[pixel][2];
                            block_bgra[pixel * 4u + 1u] = pixels[pixel][1];
                            block_bgra[pixel * 4u + 2u] = pixels[pixel][0];
                            block_bgra[pixel * 4u + 3u] = pixels[pixel][3];
                        }
                        edds_bc7_encode_block(block_bgra, refits, block);
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }
}

static void scatter_block(
    const uint8_t decoded[BLOCK_PIXELS * 4],
    uint32_t width,
    uint32_t height,
    uint32_t left,
    uint32_t top,
    uint8_t *rgba) {
    for (unsigned row = 0; row < 4; ++row) {
        if (top + row >= height) {
            break;
        }
        for (unsigned column = 0; column < 4; ++column) {
            if (left + column >= width) {
                break;
            }
            memcpy(rgba + (((size_t)(top + row) * width) + left + column) * 4u,
                decoded + (row * 4u + column) * 4u, 4);
        }
    }
}

int edds_gpu_decode(
    edds_pixel_format format,
    const uint8_t *stored,
    uint32_t stored_bytes,
    uint32_t width,
    uint32_t height,
    uint8_t *rgba) {
    const uint32_t block_bytes = edds_gpu_block_bytes(format);
    if (edds_gpu_mip_bytes(format, width, height) != stored_bytes || stored_bytes == 0) {
        return 0;
    }
    if (block_bytes == 0) {
        for (size_t pixel = 0; pixel < (size_t)width * height; ++pixel) {
            if (format == EDDS_PIXEL_R8) {
                rgba[pixel * 4u] = stored[pixel];
                rgba[pixel * 4u + 1u] = 0;
                rgba[pixel * 4u + 2u] = 0;
                rgba[pixel * 4u + 3u] = 255u;
            } else if (format == EDDS_PIXEL_RG8) {
                rgba[pixel * 4u] = stored[pixel * 2u];
                rgba[pixel * 4u + 1u] = stored[pixel * 2u + 1u];
                rgba[pixel * 4u + 2u] = 0;
                rgba[pixel * 4u + 3u] = 255u;
            } else {
                rgba[pixel * 4u] = stored[pixel * 4u + 2u];
                rgba[pixel * 4u + 1u] = stored[pixel * 4u + 1u];
                rgba[pixel * 4u + 2u] = stored[pixel * 4u];
                rgba[pixel * 4u + 3u] =
                    format == EDDS_PIXEL_BGRX8 ? 255u : stored[pixel * 4u + 3u];
            }
        }
        return 1;
    }
    {
        const uint32_t columns = (width + 3u) / 4u;
        const uint32_t rows = (height + 3u) / 4u;
        for (uint32_t row = 0; row < rows; ++row) {
            for (uint32_t column = 0; column < columns; ++column) {
                const uint8_t *block = stored + ((size_t)row * columns + column) * block_bytes;
                uint8_t decoded[BLOCK_PIXELS * 4];
                uint8_t channel[BLOCK_PIXELS];
                uint8_t second[BLOCK_PIXELS];
                switch (format) {
                    case EDDS_PIXEL_DXT1:
                        bc1_decode_block(block, 0, decoded);
                        break;
                    case EDDS_PIXEL_DXT5:
                        bc1_decode_block(block + 8, 1, decoded);
                        bc4_decode_block(block, channel);
                        for (unsigned pixel = 0; pixel < BLOCK_PIXELS; ++pixel) {
                            decoded[pixel * 4u + 3u] = channel[pixel];
                        }
                        break;
                    case EDDS_PIXEL_BC4:
                        bc4_decode_block(block, channel);
                        for (unsigned pixel = 0; pixel < BLOCK_PIXELS; ++pixel) {
                            decoded[pixel * 4u] = channel[pixel];
                            decoded[pixel * 4u + 1u] = 0;
                            decoded[pixel * 4u + 2u] = 0;
                            decoded[pixel * 4u + 3u] = 255u;
                        }
                        break;
                    case EDDS_PIXEL_BC5:
                        bc4_decode_block(block, channel);
                        bc4_decode_block(block + 8, second);
                        for (unsigned pixel = 0; pixel < BLOCK_PIXELS; ++pixel) {
                            decoded[pixel * 4u] = channel[pixel];
                            decoded[pixel * 4u + 1u] = second[pixel];
                            decoded[pixel * 4u + 2u] = 0;
                            decoded[pixel * 4u + 3u] = 255u;
                        }
                        break;
                    case EDDS_PIXEL_BC7:
                        edds_bc7_decode_block(block, decoded);
                        break;
                    default:
                        memset(decoded, 0, sizeof decoded);
                        break;
                }
                scatter_block(decoded, width, height, column * 4u, row * 4u, rgba);
            }
        }
    }
    return 1;
}
