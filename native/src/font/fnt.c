#include "font_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * FNT5: an IFF form of chunks whose sizes are big-endian and whose fields are little-endian.
 * HEAD names the font and its metrics, GLPS the runs of consecutive codes, TCRD one box per code,
 * KERN the pairs the engine finds by binary search over `(left << 16) | right`.
 */

#define TYPE_MSDF 2u
#define MOST_NAME_BYTES 254u

typedef struct bytes {
    uint8_t *data;
    size_t size;
    size_t capacity;
    int failed;
} bytes;

static void append(bytes *out, const void *data, size_t size) {
    if (out->failed) return;
    if (out->size + size > out->capacity) {
        size_t capacity = out->capacity == 0 ? 1024u : out->capacity;
        uint8_t *grown;
        while (capacity < out->size + size) capacity *= 2u;
        grown = realloc(out->data, capacity);
        if (grown == NULL) {
            out->failed = 1;
            return;
        }
        out->data = grown;
        out->capacity = capacity;
    }
    memcpy(out->data + out->size, data, size);
    out->size += size;
}

static void le16(bytes *out, uint32_t value) {
    const uint8_t data[2] = { (uint8_t)value, (uint8_t)(value >> 8) };
    append(out, data, 2);
}

static void le32(bytes *out, uint32_t value) {
    const uint8_t data[4] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24) };
    append(out, data, 4);
}

static void float32(bytes *out, float value) {
    uint32_t raw;
    memcpy(&raw, &value, sizeof raw);
    le32(out, raw);
}

static void byte8(bytes *out, uint32_t value) {
    const uint8_t data = (uint8_t)value;
    append(out, &data, 1);
}

static void be32_at(bytes *out, size_t at, size_t value) {
    if (out->failed) return;
    out->data[at] = (uint8_t)(value >> 24);
    out->data[at + 1u] = (uint8_t)(value >> 16);
    out->data[at + 2u] = (uint8_t)(value >> 8);
    out->data[at + 3u] = (uint8_t)value;
}

/** Writes a chunk header and returns where its size goes once the body is known. */
static size_t open_chunk(bytes *out, const char tag[4]) {
    const size_t at = out->size + 4u;
    append(out, tag, 4);
    le32(out, 0);
    return at;
}

static void close_chunk(bytes *out, size_t size_at) {
    be32_at(out, size_at, out->size - size_at - 4u);
}

edds_status font_fnt_write(
    const font_header *header,
    const font_entry *entries,
    size_t entry_count,
    const font_pair *pairs,
    size_t pair_count,
    uint8_t **data,
    size_t *size,
    uint32_t *range_count,
    edds_error *error
) {
    bytes out = { NULL, 0, 0, 0 };
    const size_t name_size = strlen(header->name);
    size_t ranges = 0;
    size_t chunk;
    size_t ranges_at;
    *data = NULL;
    *size = 0;
    if (name_size == 0 || name_size > MOST_NAME_BYTES) {
        font_fail(error, "invalid-font-name", "A font name is 1 to %u bytes.", MOST_NAME_BYTES);
        return EDDS_INVALID_INPUT;
    }
    append(&out, "FORM", 4);
    le32(&out, 0);
    append(&out, "FNT5", 4);

    chunk = open_chunk(&out, "HEAD");
    le32(&out, (uint32_t)name_size + 1u);
    append(&out, header->name, name_size + 1u);
    le32(&out, header->size);
    le32(&out, 0);
    byte8(&out, TYPE_MSDF);
    le32(&out, header->cell);
    float32(&out, header->cap_height);
    float32(&out, (float)header->size);
    le16(&out, FONT_FIELD_R);
    /* Bold and italic: the widget sets both before every draw, so the file's are only a start. */
    byte8(&out, 0);
    byte8(&out, 0);
    float32(&out, (float)header->size);
    close_chunk(&out, chunk);

    chunk = open_chunk(&out, "GLPS");
    /* Eight bytes the engine skips; Workbench leaves whatever its memory held there. */
    le32(&out, 0);
    le32(&out, 0);
    le32(&out, (uint32_t)entry_count);
    ranges_at = out.size;
    le32(&out, 0);
    for (size_t at = 0; at < entry_count;) {
        size_t run = 1;
        while (at + run < entry_count && entries[at + run].code == entries[at].code + run && run < 0xFFFFu) ++run;
        le32(&out, entries[at].code);
        le16(&out, (uint32_t)run);
        le16(&out, 0);
        ++ranges;
        at += run;
    }
    if (!out.failed) {
        out.data[ranges_at] = (uint8_t)ranges;
        out.data[ranges_at + 1u] = (uint8_t)(ranges >> 8);
        out.data[ranges_at + 2u] = (uint8_t)(ranges >> 16);
        out.data[ranges_at + 3u] = (uint8_t)(ranges >> 24);
    }
    close_chunk(&out, chunk);

    chunk = open_chunk(&out, "TCRD");
    for (size_t at = 0; at < entry_count; ++at) {
        le16(&out, entries[at].x);
        le16(&out, entries[at].y);
        le16(&out, entries[at].width);
        le16(&out, entries[at].height);
        le16(&out, (uint16_t)entries[at].box_x);
        le16(&out, (uint16_t)entries[at].box_y);
        le16(&out, (uint16_t)entries[at].advance);
    }
    close_chunk(&out, chunk);

    chunk = open_chunk(&out, "KERN");
    for (size_t at = 0; at < pair_count; ++at) {
        le16(&out, pairs[at].left);
        le16(&out, pairs[at].right);
        le32(&out, (uint32_t)pairs[at].value);
    }
    close_chunk(&out, chunk);

    be32_at(&out, 4, out.size - 8u);
    if (out.failed) {
        free(out.data);
        font_fail(error, "allocation-failed", "Memory for the FNT file could not be allocated.");
        return EDDS_INTERNAL_FAILURE;
    }
    *data = out.data;
    *size = out.size;
    *range_count = (uint32_t)ranges;
    return EDDS_OK;
}

/* --- Reading ---------------------------------------------------------------------------------- */

static uint32_t be32(const uint8_t *at) {
    return ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16) | ((uint32_t)at[2] << 8) | at[3];
}

static uint32_t read32(const uint8_t *at) {
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

static uint16_t read16(const uint8_t *at) {
    return (uint16_t)(at[0] | (at[1] << 8));
}

static float read_float(const uint8_t *at) {
    const uint32_t raw = read32(at);
    float value;
    memcpy(&value, &raw, sizeof value);
    return value;
}

static edds_status malformed(edds_error *error, const char *what) {
    font_fail(error, "malformed-fnt", "The FNT file is malformed: %s.", what);
    return EDDS_INVALID_INPUT;
}

static edds_status read_head(const uint8_t *chunk, uint32_t size, font_info *info, edds_error *error) {
    uint32_t name_size;
    const uint8_t *at;
    if (size < 4u) return malformed(error, "HEAD is truncated");
    name_size = read32(chunk);
    if (name_size == 0 || name_size > sizeof info->name || size < 4u + name_size + 29u) {
        return malformed(error, "the HEAD name");
    }
    if (chunk[4u + name_size - 1u] != 0) return malformed(error, "the HEAD name is not terminated");
    memcpy(info->name, chunk + 4, name_size);
    at = chunk + 4u + name_size;
    info->size = (int32_t)read32(at);
    info->type = at[8];
    info->cell = (int32_t)read32(at + 9);
    info->cap_height = read_float(at + 13);
    info->line_height = read_float(at + 17);
    info->r = (int16_t)read16(at + 21);
    info->bold = at[23];
    info->italic = at[24];
    info->c = read_float(at + 25);
    if (!isfinite(info->cap_height) || !isfinite(info->line_height) || !isfinite(info->c)) {
        return malformed(error, "a HEAD metric is not a finite number");
    }
    return EDDS_OK;
}

static edds_status read_glps(const uint8_t *chunk, uint32_t size, font_info *info, edds_error *error) {
    uint64_t total = 0;
    if (size < 16u) return malformed(error, "GLPS is truncated");
    info->glyph_count = read32(chunk + 8);
    info->range_count = read32(chunk + 12);
    if ((uint64_t)size != 16u + 8u * (uint64_t)info->range_count) return malformed(error, "the GLPS ranges");
    info->ranges = malloc((info->range_count == 0 ? 1u : info->range_count) * sizeof *info->ranges);
    if (info->ranges == NULL) {
        font_fail(error, "allocation-failed", "Memory for the FNT ranges could not be allocated.");
        return EDDS_INTERNAL_FAILURE;
    }
    for (uint32_t at = 0; at < info->range_count; ++at) {
        info->ranges[at].first = read32(chunk + 16u + 8u * at);
        info->ranges[at].count = read16(chunk + 20u + 8u * at);
        total += info->ranges[at].count;
    }
    if (total != info->glyph_count) return malformed(error, "the GLPS ranges do not add up to the glyph count");
    return EDDS_OK;
}

edds_status font_inspect(FILE *input, font_info *info, edds_error *error) {
    uint8_t *data = NULL;
    long length;
    size_t size;
    size_t at = 12;
    int head = 0, glyphs = 0, boxes = 0;
    uint32_t box_bytes = 0;
    edds_status status = EDDS_OK;
    if (input == NULL || info == NULL) {
        font_fail(error, "invalid-api-argument", "The FNT input and value are required.");
        return EDDS_INTERNAL_FAILURE;
    }
    memset(info, 0, sizeof *info);
    if (fseek(input, 0, SEEK_END) != 0 || (length = ftell(input)) < 0 || fseek(input, 0, SEEK_SET) != 0 ||
        (uint64_t)length > FONT_MAX_FILE_BYTES) {
        font_fail(error, "fnt-size-limit", "The FNT file could not be measured within %u bytes.", FONT_MAX_FILE_BYTES);
        return EDDS_INVALID_INPUT;
    }
    size = (size_t)length;
    data = malloc(size == 0 ? 1u : size);
    if (data == NULL) {
        font_fail(error, "allocation-failed", "Memory for the FNT file could not be allocated.");
        return EDDS_INTERNAL_FAILURE;
    }
    if (fread(data, 1, size, input) != size) {
        free(data);
        font_fail(error, "fnt-read-failed", "The FNT file could not be read completely.");
        return EDDS_INVALID_INPUT;
    }
    if (size < 12u || memcmp(data, "FORM", 4) != 0 || memcmp(data + 8, "FNT5", 4) != 0 ||
        (uint64_t)be32(data + 4) + 8u != size) {
        free(data);
        return malformed(error, "it is not one FORM of type FNT5");
    }
    while (at < size && status == EDDS_OK) {
        uint32_t chunk_size;
        if (size - at < 8u) {
            status = malformed(error, "a chunk header is truncated");
            break;
        }
        chunk_size = be32(data + at + 4u);
        if (chunk_size > size - at - 8u) {
            status = malformed(error, "a chunk runs past the file");
            break;
        }
        if (memcmp(data + at, "HEAD", 4) == 0) {
            status = head++ ? malformed(error, "HEAD occurs twice") : read_head(data + at + 8u, chunk_size, info, error);
        } else if (memcmp(data + at, "GLPS", 4) == 0) {
            status = glyphs++ ? malformed(error, "GLPS occurs twice") : read_glps(data + at + 8u, chunk_size, info, error);
        } else if (memcmp(data + at, "TCRD", 4) == 0) {
            ++boxes;
            box_bytes = chunk_size;
        } else if (memcmp(data + at, "KERN", 4) == 0) {
            if (chunk_size % 8u != 0) status = malformed(error, "KERN is not whole pairs");
            info->pair_count = chunk_size / 8u;
        }
        at += 8u + chunk_size;
    }
    if (status == EDDS_OK && (head != 1 || glyphs != 1 || boxes != 1)) {
        status = malformed(error, "it needs one HEAD, one GLPS and one TCRD");
    }
    if (status == EDDS_OK && (uint64_t)box_bytes != 14u * (uint64_t)info->glyph_count) {
        status = malformed(error, "TCRD does not hold one box per glyph");
    }
    free(data);
    if (status != EDDS_OK) font_info_free(info);
    return status;
}

void font_info_free(font_info *info) {
    if (info == NULL) return;
    free(info->ranges);
    info->ranges = NULL;
}
