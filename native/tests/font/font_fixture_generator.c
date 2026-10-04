#include "font_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Characters the fixture font lacks: they must be reported as missing, never drawn. */
static const uint32_t absent[] = { 0x0416, 0x0078 };

static size_t utf8_of(uint32_t code, char out[4]) {
    if (code < 0x80u) {
        out[0] = (char)code;
        return 1;
    }
    if (code < 0x800u) {
        out[0] = (char)(0xC0u | (code >> 6));
        out[1] = (char)(0x80u | (code & 0x3Fu));
        return 2;
    }
    if (code < 0x10000u) {
        out[0] = (char)(0xE0u | (code >> 12));
        out[1] = (char)(0x80u | ((code >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (code & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (code >> 18));
    out[1] = (char)(0x80u | ((code >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((code >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (code & 0x3Fu));
    return 4;
}

static int write_file(const char *folder, const char *name, const void *data, size_t size) {
    char path[4096];
    FILE *file;
    int ok;
    if (snprintf(path, sizeof path, "%s/%s", folder, name) >= (int)sizeof path) {
        return 0;
    }
#ifdef _WIN32
    if (fopen_s(&file, path, "wb") != 0) {
        file = NULL;
    }
#else
    file = fopen(path, "wb");
#endif
    if (file == NULL) {
        return 0;
    }
    ok = fwrite(data, 1, size, file) == size;
    return fclose(file) == 0 && ok;
}

static int write_font(const char *folder, const char *name, font_fixture_variant variant) {
    test_bytes font = font_fixture(variant);
    const int ok = font.data != NULL && write_file(folder, name, font.data, font.size);
    fixture_free(font);
    return ok;
}

/*
 * The set as a modder writes one: a line of Latin, a line of everything else, a space and a line
 * break that only separate, and two characters the font cannot provide.
 */
static int write_charset(const char *folder) {
    const uint32_t *codes = NULL;
    const size_t count = font_fixture_codes(FONT_FIXTURE_GPOS, &codes);
    char text[1024];
    size_t size = 0;
    for (size_t at = 0; at < count; ++at) {
        if (codes[at] == 0x0020u || codes[at] == 0x00A0u) {
            continue;
        }
        if (at > 0 && codes[at] >= 0x80u && codes[at - 1] < 0x80u) {
            text[size++] = '\n';
        }
        size += utf8_of(codes[at], text + size);
    }
    text[size++] = ' ';
    for (size_t at = 0; at < sizeof absent / sizeof absent[0]; ++at) {
        size += utf8_of(absent[at], text + size);
    }
    text[size++] = '\r';
    text[size++] = '\n';
    /* No-break space is a character a text can hold, so it is a member, not a separator. */
    size += utf8_of(0x00A0u, text + size);
    text[size++] = '\n';
    return write_file(folder, "fixture.charset.txt", text, size);
}

static int write_crowded_charset(const char *folder) {
    char *text = malloc((size_t)FONT_FIXTURE_CROWDED_COUNT * 3u);
    size_t size = 0;
    int ok;
    if (text == NULL) {
        return 0;
    }
    for (uint32_t at = 0; at < FONT_FIXTURE_CROWDED_COUNT; ++at) {
        size += utf8_of(FONT_FIXTURE_CROWDED_FIRST + at, text + size);
    }
    ok = write_file(folder, "crowded.charset.txt", text, size);
    free(text);
    return ok;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fputs("usage: enfusion-font-fixture OUTPUT_FOLDER\n", stderr);
        return 2;
    }
    return write_font(argv[1], "fixture-gpos.ttf", FONT_FIXTURE_GPOS) &&
            write_font(argv[1], "fixture-kern.ttf", FONT_FIXTURE_KERN) &&
            write_font(argv[1], "fixture-cff.otf", FONT_FIXTURE_CFF) &&
            write_font(argv[1], "fixture-variable.ttf", FONT_FIXTURE_VARIABLE) &&
            write_font(argv[1], "fixture-crowded.ttf", FONT_FIXTURE_CROWDED) &&
            write_charset(argv[1]) &&
            write_crowded_charset(argv[1])
        ? 0
        : 1;
}
