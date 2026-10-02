#include "bytes.h"
#include "font_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * Pair kerning as a shaper would apply it between two glyphs of a BMP text, reduced to what the
 * engine's KERN chunk can hold: one advance shift for the first character of each pair.
 */

static edds_status malformed(edds_error *error, const char *what) {
    font_fail(error, "malformed-kerning", "The font's kerning is malformed: %s.", what);
    return EDDS_INVALID_INPUT;
}

static edds_status too_many_pairs(edds_error *error) {
    font_fail(error, "kerning-limit", "The font has more than %zu kerning pairs in this set.", FONT_MAX_PAIRS);
    return EDDS_UNSUPPORTED_FORMAT;
}

static unsigned bits_set(unsigned value) {
    unsigned count = 0;
    for (; value != 0; value &= value - 1u) ++count;
    return count;
}

/* --- GPOS ------------------------------------------------------------------------------------- */

/** One PairPos subtable, checked once so a lookup never reads past it. */
typedef struct pair_subtable {
    uint32_t lookup;
    unsigned format;
    uint32_t base;
    uint32_t coverage;
    uint32_t first_size;
    uint32_t record_size;
    /* Byte offset of XAdvance inside the first value record, or -1 when it carries none. */
    int x_advance;
    uint32_t set_count;
    uint32_t class_first;
    uint32_t class_second;
    uint32_t class_first_count;
    uint32_t class_second_count;
} pair_subtable;

typedef struct gpos_reader {
    const uint8_t *data;
    uint32_t length;
    pair_subtable *subtables;
    size_t count;
    size_t capacity;
} gpos_reader;

/** Index of a glyph in a Coverage table, or -1. */
static int32_t coverage_index(const gpos_reader *gpos, uint32_t coverage, uint32_t glyph) {
    const uint8_t *table = gpos->data + coverage;
    const unsigned format = u16(table);
    uint32_t low = 0, high = u16(table + 2);
    while (low < high) {
        const uint32_t middle = low + (high - low) / 2u;
        if (format == 1u) {
            const uint32_t found = u16(table + 4u + 2u * middle);
            if (glyph < found) high = middle;
            else if (glyph > found) low = middle + 1u;
            else return (int32_t)middle;
        } else {
            const uint8_t *range = table + 4u + 6u * middle;
            if (glyph < u16(range)) high = middle;
            else if (glyph > u16(range + 2)) low = middle + 1u;
            else return (int32_t)(u16(range + 4) + (glyph - u16(range)));
        }
    }
    return -1;
}

static int valid_coverage(const gpos_reader *gpos, uint32_t coverage) {
    unsigned format;
    if (!inside(gpos->length, coverage, 4u)) return 0;
    format = u16(gpos->data + coverage);
    if (format == 1u) return inside(gpos->length, coverage + 4u, 2u * (uint64_t)u16(gpos->data + coverage + 2));
    if (format == 2u) return inside(gpos->length, coverage + 4u, 6u * (uint64_t)u16(gpos->data + coverage + 2));
    return 0;
}

static uint32_t class_of(const gpos_reader *gpos, uint32_t class_def, uint32_t glyph) {
    const uint8_t *table = gpos->data + class_def;
    if (class_def == 0) return 0;
    if (u16(table) == 1u) {
        const uint32_t first = u16(table + 2), count = u16(table + 4);
        return glyph >= first && glyph - first < count ? u16(table + 6u + 2u * (glyph - first)) : 0u;
    } else {
        uint32_t low = 0, high = u16(table + 2);
        while (low < high) {
            const uint32_t middle = low + (high - low) / 2u;
            const uint8_t *range = table + 4u + 6u * middle;
            if (glyph < u16(range)) high = middle;
            else if (glyph > u16(range + 2)) low = middle + 1u;
            else return u16(range + 4);
        }
        return 0;
    }
}

static int valid_class_def(const gpos_reader *gpos, uint32_t class_def) {
    unsigned format;
    if (class_def == 0) return 1;
    if (!inside(gpos->length, class_def, 6u)) return 0;
    format = u16(gpos->data + class_def);
    if (format == 1u) return inside(gpos->length, class_def + 6u, 2u * (uint64_t)u16(gpos->data + class_def + 4));
    if (format == 2u) return inside(gpos->length, class_def + 4u, 6u * (uint64_t)u16(gpos->data + class_def + 2));
    return 0;
}

static edds_status add_pair_subtable(gpos_reader *gpos, uint32_t lookup, uint32_t base, edds_error *error) {
    pair_subtable subtable;
    unsigned first_format, second_format;
    memset(&subtable, 0, sizeof subtable);
    if (!inside(gpos->length, base, 10u)) return malformed(error, "a PairPos subtable is truncated");
    subtable.lookup = lookup;
    subtable.base = base;
    subtable.format = u16(gpos->data + base);
    subtable.coverage = base + u16(gpos->data + base + 2);
    first_format = u16(gpos->data + base + 4) & 0xFFu;
    second_format = u16(gpos->data + base + 6) & 0xFFu;
    subtable.first_size = 2u * bits_set(first_format);
    subtable.record_size = subtable.first_size + 2u * bits_set(second_format);
    subtable.x_advance = (first_format & 0x0004u) != 0 ? (int)(2u * bits_set(first_format & 0x0003u)) : -1;
    if (!valid_coverage(gpos, subtable.coverage)) return malformed(error, "a PairPos coverage");
    if (subtable.format == 1u) {
        subtable.set_count = u16(gpos->data + base + 8);
        if (!inside(gpos->length, base + 10u, 2u * (uint64_t)subtable.set_count)) {
            return malformed(error, "a PairPos set list");
        }
        for (uint32_t at = 0; at < subtable.set_count; ++at) {
            const uint32_t set = base + u16(gpos->data + base + 10u + 2u * at);
            if (!inside(gpos->length, set, 2u) ||
                !inside(gpos->length, set + 2u, (2u + (uint64_t)subtable.record_size) * u16(gpos->data + set))) {
                return malformed(error, "a PairPos set");
            }
        }
    } else if (subtable.format == 2u) {
        if (!inside(gpos->length, base, 16u)) return malformed(error, "a class PairPos subtable is truncated");
        subtable.class_first = u16(gpos->data + base + 8) == 0 ? 0u : base + u16(gpos->data + base + 8);
        subtable.class_second = u16(gpos->data + base + 10) == 0 ? 0u : base + u16(gpos->data + base + 10);
        subtable.class_first_count = u16(gpos->data + base + 12);
        subtable.class_second_count = u16(gpos->data + base + 14);
        if (!valid_class_def(gpos, subtable.class_first) || !valid_class_def(gpos, subtable.class_second) ||
            !inside(gpos->length, base + 16u, (uint64_t)subtable.class_first_count *
                subtable.class_second_count * subtable.record_size)) {
            return malformed(error, "a class PairPos subtable");
        }
    } else {
        return malformed(error, "an unknown PairPos format");
    }
    if (gpos->count == gpos->capacity) {
        const size_t capacity = gpos->capacity == 0 ? 16u : gpos->capacity * 2u;
        pair_subtable *grown = realloc(gpos->subtables, capacity * sizeof *grown);
        if (grown == NULL) {
            font_fail(error, "allocation-failed", "Memory for the kerning lookups could not be allocated.");
            return EDDS_INTERNAL_FAILURE;
        }
        gpos->subtables = grown;
        gpos->capacity = capacity;
    }
    gpos->subtables[gpos->count++] = subtable;
    return EDDS_OK;
}

/**
 * Marks the lookups of every `kern` feature the default language system of `script` enables: what
 * text in that script with no language set is kerned by. A language's own system is a tailoring
 * the engine has no way to ask for.
 */
static edds_status mark_script_lookups(
    const gpos_reader *gpos,
    uint32_t script,
    uint32_t features,
    uint8_t *marked,
    edds_error *error
) {
    uint32_t system, indices, feature_count;
    if (!inside(gpos->length, script, 4u)) return malformed(error, "a script table");
    if (u16(gpos->data + script) == 0) return EDDS_OK;
    system = script + u16(gpos->data + script);
    if (!inside(gpos->length, system, 6u)) return malformed(error, "a language system");
    indices = u16(gpos->data + system + 4);
    if (!inside(gpos->length, system + 6u, 2u * (uint64_t)indices)) return malformed(error, "a language system");
    if (!inside(gpos->length, features, 2u)) return malformed(error, "the feature list");
    feature_count = u16(gpos->data + features);
    if (!inside(gpos->length, features + 2u, 6u * (uint64_t)feature_count)) return malformed(error, "the feature list");
    for (uint32_t index = 0; index <= indices; ++index) {
        /* The required feature, when there is one, applies like any listed one. */
        const uint32_t feature = index == indices ? u16(gpos->data + system + 2) : u16(gpos->data + system + 6u + 2u * index);
        uint32_t table, lookups;
        if (feature == 0xFFFFu && index == indices) continue;
        if (feature >= feature_count) return malformed(error, "a language system names a missing feature");
        if (u32(gpos->data + features + 2u + 6u * feature) != TAG('k', 'e', 'r', 'n')) continue;
        table = features + u16(gpos->data + features + 2u + 6u * feature + 4u);
        if (!inside(gpos->length, table, 4u)) return malformed(error, "a feature table");
        lookups = u16(gpos->data + table + 2);
        if (!inside(gpos->length, table + 4u, 2u * (uint64_t)lookups)) return malformed(error, "a feature table");
        for (uint32_t lookup = 0; lookup < lookups; ++lookup) marked[u16(gpos->data + table + 4u + 2u * lookup)] = 1;
    }
    return EDDS_OK;
}

static edds_status read_gpos(gpos_reader *gpos, edds_error *error) {
    uint32_t scripts, features, lookups, script_count, lookup_count;
    uint8_t *marked;
    edds_status status = EDDS_OK;
    if (!inside(gpos->length, 0, 10u) || u16(gpos->data) != 1u) return malformed(error, "the GPOS header");
    scripts = u16(gpos->data + 4);
    features = u16(gpos->data + 6);
    lookups = u16(gpos->data + 8);
    if (!inside(gpos->length, scripts, 2u) || !inside(gpos->length, lookups, 2u)) return malformed(error, "the GPOS header");
    script_count = u16(gpos->data + scripts);
    lookup_count = u16(gpos->data + lookups);
    if (!inside(gpos->length, scripts + 2u, 6u * (uint64_t)script_count) ||
        !inside(gpos->length, lookups + 2u, 2u * (uint64_t)lookup_count)) {
        return malformed(error, "the GPOS lists");
    }
    marked = calloc(65536u, 1);
    if (marked == NULL) {
        font_fail(error, "allocation-failed", "Memory for the kerning lookups could not be allocated.");
        return EDDS_INTERNAL_FAILURE;
    }
    for (uint32_t at = 0; at < script_count && status == EDDS_OK; ++at) {
        const uint32_t tag = u32(gpos->data + scripts + 2u + 6u * at);
        if (tag != TAG('D', 'F', 'L', 'T') && tag != TAG('l', 'a', 't', 'n') && tag != TAG('c', 'y', 'r', 'l')) continue;
        status = mark_script_lookups(gpos, scripts + u16(gpos->data + scripts + 2u + 6u * at + 4u), features, marked, error);
    }
    /* Lookups apply in LookupList order, whichever feature listed them first. */
    for (uint32_t index = 0; index < lookup_count && status == EDDS_OK; ++index) {
        uint32_t lookup, type, subtable_count;
        if (!marked[index]) continue;
        lookup = lookups + u16(gpos->data + lookups + 2u + 2u * index);
        if (!inside(gpos->length, lookup, 6u)) {
            status = malformed(error, "a lookup table");
            break;
        }
        type = u16(gpos->data + lookup);
        subtable_count = u16(gpos->data + lookup + 4);
        if (!inside(gpos->length, lookup + 6u, 2u * (uint64_t)subtable_count)) {
            status = malformed(error, "a lookup table");
            break;
        }
        for (uint32_t at = 0; at < subtable_count && status == EDDS_OK; ++at) {
            uint32_t subtable = lookup + u16(gpos->data + lookup + 6u + 2u * at);
            if (type == 9u) {
                if (!inside(gpos->length, subtable, 8u) || u16(gpos->data + subtable) != 1u) {
                    status = malformed(error, "an Extension subtable");
                    break;
                }
                if (u16(gpos->data + subtable + 2) != 2u) continue;
                if ((uint64_t)subtable + u32(gpos->data + subtable + 4) > gpos->length) {
                    status = malformed(error, "an Extension subtable");
                    break;
                }
                subtable += u32(gpos->data + subtable + 4);
            } else if (type != 2u) {
                break;
            }
            status = add_pair_subtable(gpos, index, subtable, error);
        }
    }
    free(marked);
    return status;
}

/** XAdvance of the first glyph if this subtable applies to the pair; 0 if it does not apply. */
static int pair_value(const gpos_reader *gpos, const pair_subtable *subtable, int32_t covered,
    uint32_t first, uint32_t second, int32_t *value) {
    if (subtable->format == 1u) {
        uint32_t set, low = 0, high;
        if ((uint32_t)covered >= subtable->set_count) return 0;
        set = subtable->base + u16(gpos->data + subtable->base + 10u + 2u * (uint32_t)covered);
        high = u16(gpos->data + set);
        while (low < high) {
            const uint32_t middle = low + (high - low) / 2u;
            const uint8_t *record = gpos->data + set + 2u + (2u + subtable->record_size) * middle;
            if (second < u16(record)) high = middle;
            else if (second > u16(record)) low = middle + 1u;
            else {
                *value = subtable->x_advance < 0 ? 0 : s16(record + 2 + subtable->x_advance);
                return 1;
            }
        }
        return 0;
    } else {
        const uint32_t row = class_of(gpos, subtable->class_first, first);
        const uint32_t column = class_of(gpos, subtable->class_second, second);
        const uint8_t *record;
        if (row >= subtable->class_first_count || column >= subtable->class_second_count) return 0;
        record = gpos->data + subtable->base + 16u +
            ((uint64_t)row * subtable->class_second_count + column) * subtable->record_size;
        *value = subtable->x_advance < 0 ? 0 : s16(record + subtable->x_advance);
        return 1;
    }
}

/* --- kern ------------------------------------------------------------------------------------- */

typedef struct kern_entry {
    uint32_t left;
    uint32_t right;
    uint32_t order;
    int32_t value;
    int replace;
} kern_entry;

static int entry_order(const void *a, const void *b) {
    const kern_entry *left = a, *right = b;
    if (left->left != right->left) return left->left < right->left ? -1 : 1;
    if (left->right != right->right) return left->right < right->right ? -1 : 1;
    return (left->order > right->order) - (left->order < right->order);
}

typedef struct glyph_slot {
    uint32_t glyph;
    uint32_t index;
} glyph_slot;

static int slot_order(const void *a, const void *b) {
    const glyph_slot *left = a, *right = b;
    if (left->glyph != right->glyph) return left->glyph < right->glyph ? -1 : 1;
    return (left->index > right->index) - (left->index < right->index);
}

/** First slot of `glyph` in the sorted slots, or `count`. */
static size_t first_slot(const glyph_slot *slots, size_t count, uint32_t glyph) {
    size_t low = 0, high = count;
    while (low < high) {
        const size_t middle = low + (high - low) / 2u;
        if (slots[middle].glyph < glyph) low = middle + 1u;
        else high = middle;
    }
    return low;
}

static edds_status read_kern(
    const uint8_t *table,
    uint32_t length,
    const glyph_slot *slots,
    size_t slot_count,
    kern_entry **entries,
    size_t *entry_count,
    edds_error *error
) {
    const int apple = length >= 8u && u32(table) == 0x00010000u;
    uint32_t tables, at;
    size_t capacity = 0;
    *entries = NULL;
    *entry_count = 0;
    if (length < 4u) return malformed(error, "the kern header");
    if (!apple && u16(table) != 0) return malformed(error, "an unknown kern version");
    tables = apple ? u32(table + 4) : u16(table + 2);
    at = apple ? 8u : 4u;
    for (uint32_t index = 0; index < tables; ++index) {
        uint32_t header = apple ? 8u : 6u, size, format, pairs = 0;
        int usable, replace;
        if (!inside(length, at, header)) return malformed(error, "a kern subtable header");
        if (apple) {
            const unsigned coverage = u16(table + at + 4);
            size = u32(table + at);
            format = coverage & 0xFFu;
            usable = (coverage & 0xE000u) == 0;
            replace = 0;
        } else {
            const unsigned coverage = u16(table + at + 4);
            size = u16(table + at + 2);
            format = coverage >> 8;
            usable = (coverage & 0x0007u) == 0x0001u;
            replace = (coverage & 0x0008u) != 0;
        }
        if (format == 0u) {
            if (!inside(length, at + header, 8u)) return malformed(error, "a kern pair list");
            pairs = u16(table + at + header);
            /* A large format 0 subtable overflows its 16-bit length; its pair count is authoritative. */
            size = header + 8u + 6u * pairs;
            if (!inside(length, at, size)) return malformed(error, "a kern pair list");
        }
        if (size < header || !inside(length, at, size)) return malformed(error, "a kern subtable");
        if (format == 0u && usable) {
            const uint8_t *pair = table + at + header + 8u;
            for (uint32_t number = 0; number < pairs; ++number, pair += 6) {
                const uint32_t left_glyph = u16(pair), right_glyph = u16(pair + 2);
                const size_t first_left = first_slot(slots, slot_count, left_glyph);
                const size_t first_right = first_slot(slots, slot_count, right_glyph);
                for (size_t left = first_left; left < slot_count && slots[left].glyph == left_glyph; ++left) {
                    for (size_t right = first_right; right < slot_count && slots[right].glyph == right_glyph; ++right) {
                        if (*entry_count == capacity) {
                            kern_entry *grown;
                            if (capacity >= FONT_MAX_PAIRS) return too_many_pairs(error);
                            capacity = capacity == 0 ? 256u : capacity * 2u;
                            grown = realloc(*entries, capacity * sizeof *grown);
                            if (grown == NULL) {
                                font_fail(error, "allocation-failed", "Memory for the kern pairs could not be allocated.");
                                return EDDS_INTERNAL_FAILURE;
                            }
                            *entries = grown;
                        }
                        (*entries)[*entry_count].left = slots[left].index;
                        (*entries)[*entry_count].right = slots[right].index;
                        (*entries)[*entry_count].order = index;
                        (*entries)[*entry_count].value = s16(pair + 4);
                        (*entries)[*entry_count].replace = replace;
                        ++*entry_count;
                    }
                }
            }
        }
        at += size;
    }
    return EDDS_OK;
}

/* --- Both ------------------------------------------------------------------------------------- */

typedef struct pair_list {
    font_pair *pairs;
    size_t count;
    size_t capacity;
} pair_list;

static edds_status keep_pair(pair_list *list, uint32_t left, uint32_t right, double units, double scale, edds_error *error) {
    const double rounded = units * scale >= 0 ? floor(units * scale + 0.5) : -floor(-units * scale + 0.5);
    if (rounded == 0 || left > 0xFFFFu || right > 0xFFFFu) return EDDS_OK;
    if (list->count == list->capacity) {
        font_pair *grown;
        if (list->capacity >= FONT_MAX_PAIRS) return too_many_pairs(error);
        list->capacity = list->capacity == 0 ? 256u : list->capacity * 2u;
        grown = realloc(list->pairs, list->capacity * sizeof *grown);
        if (grown == NULL) {
            font_fail(error, "allocation-failed", "Memory for the kerning pairs could not be allocated.");
            return EDDS_INTERNAL_FAILURE;
        }
        list->pairs = grown;
    }
    list->pairs[list->count].left = (uint16_t)left;
    list->pairs[list->count].right = (uint16_t)right;
    list->pairs[list->count].value = (int32_t)rounded;
    ++list->count;
    return EDDS_OK;
}

static edds_status gpos_pairs(
    const font_face *face,
    const uint32_t *codes,
    const uint32_t *glyphs,
    size_t count,
    double scale,
    pair_list *list,
    edds_error *error
) {
    gpos_reader gpos = { face->data + face->gpos.offset, face->gpos.length, NULL, 0, 0 };
    int64_t *totals = NULL;
    uint8_t *done = NULL;
    edds_status status = read_gpos(&gpos, error);
    if (status == EDDS_OK && gpos.count != 0) {
        totals = malloc(count * sizeof *totals);
        done = malloc(count);
        if (totals == NULL || done == NULL) {
            font_fail(error, "allocation-failed", "Memory for the kerning pairs could not be allocated.");
            status = EDDS_INTERNAL_FAILURE;
        }
    }
    for (size_t left = 0; status == EDDS_OK && gpos.count != 0 && left < count; ++left) {
        if (codes[left] > 0xFFFFu || glyphs[left] == 0) continue;
        memset(totals, 0, count * sizeof *totals);
        for (size_t at = 0; at < gpos.count;) {
            const uint32_t lookup = gpos.subtables[at].lookup;
            memset(done, 0, count);
            /* Within one lookup the first subtable that applies to a pair is the only one. */
            for (; at < gpos.count && gpos.subtables[at].lookup == lookup; ++at) {
                const pair_subtable *subtable = &gpos.subtables[at];
                const int32_t covered = coverage_index(&gpos, subtable->coverage, glyphs[left]);
                if (covered < 0) continue;
                for (size_t right = 0; right < count; ++right) {
                    int32_t value = 0;
                    if (done[right] || codes[right] > 0xFFFFu || glyphs[right] == 0) continue;
                    if (pair_value(&gpos, subtable, covered, glyphs[left], glyphs[right], &value)) {
                        totals[right] += value;
                        done[right] = 1;
                    }
                }
            }
        }
        for (size_t right = 0; status == EDDS_OK && right < count; ++right) {
            if (totals[right] != 0) status = keep_pair(list, codes[left], codes[right], (double)totals[right], scale, error);
        }
    }
    free(totals);
    free(done);
    free(gpos.subtables);
    return status;
}

static edds_status kern_pairs(
    const font_face *face,
    const uint32_t *codes,
    const uint32_t *glyphs,
    size_t count,
    double scale,
    pair_list *list,
    edds_error *error
) {
    glyph_slot *slots = malloc((count == 0 ? 1u : count) * sizeof *slots);
    kern_entry *entries = NULL;
    size_t slot_count = 0, entry_count = 0;
    edds_status status;
    if (slots == NULL) {
        font_fail(error, "allocation-failed", "Memory for the kerning pairs could not be allocated.");
        return EDDS_INTERNAL_FAILURE;
    }
    for (size_t at = 0; at < count; ++at) {
        if (codes[at] > 0xFFFFu || glyphs[at] == 0) continue;
        slots[slot_count].glyph = glyphs[at];
        slots[slot_count].index = (uint32_t)at;
        ++slot_count;
    }
    qsort(slots, slot_count, sizeof *slots, slot_order);
    status = read_kern(face->data + face->kern.offset, face->kern.length, slots, slot_count,
        &entries, &entry_count, error);
    if (status == EDDS_OK) {
        qsort(entries, entry_count, sizeof *entries, entry_order);
        for (size_t at = 0; at < entry_count && status == EDDS_OK;) {
            int64_t total = 0;
            const size_t first = at;
            for (; at < entry_count && entries[at].left == entries[first].left &&
                entries[at].right == entries[first].right; ++at) {
                total = entries[at].replace ? entries[at].value : total + entries[at].value;
            }
            status = keep_pair(list, codes[entries[first].left], codes[entries[first].right], (double)total, scale, error);
        }
    }
    free(entries);
    free(slots);
    return status;
}

edds_status font_face_kerning(
    const font_face *face,
    const uint32_t *codes,
    const uint32_t *glyphs,
    size_t count,
    double scale,
    font_pair **pairs,
    size_t *pair_count,
    edds_error *error
) {
    pair_list list = { NULL, 0, 0 };
    edds_status status = EDDS_OK;
    *pairs = NULL;
    *pair_count = 0;
    /* Codes ascend, and both readers visit them in that order, so the keys already ascend. */
    if (face->gpos.present) status = gpos_pairs(face, codes, glyphs, count, scale, &list, error);
    else if (face->kern.present) status = kern_pairs(face, codes, glyphs, count, scale, &list, error);
    if (status != EDDS_OK) {
        free(list.pairs);
        return status;
    }
    *pairs = list.pairs;
    *pair_count = list.count;
    return EDDS_OK;
}
