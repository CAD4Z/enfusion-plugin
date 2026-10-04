/*
 * Tests of the font core called directly: recipes, character sets, fonts generated from the
 * fixture or refused, and the FNT files they produce read back by inspection.
 */
#include <font/font.h>

#include "font_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Fails the test it is used in when the condition does not hold: prints the file, the line and the
 * condition, and returns 0 from the test.
 */
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            return 0; \
        } \
    } while (0)

/** A new temporary file open for reading and writing, or NULL when none could be made. */
static FILE *temporary(void) {
#ifdef _WIN32
    FILE *file = NULL;

    return tmpfile_s(&file) == 0 ? file : NULL;
#else
    return tmpfile();
#endif
}

/** A temporary file holding text, rewound to its start; NULL on failure. The caller closes it. */
static FILE *text_file(const char *text) {
    FILE *file = temporary();

    if (file == NULL) {
        return NULL;
    }

    if (fputs(text, file) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }

    return file;
}

/**
 * The whole of a stream, NUL-terminated, for comparing written text. NULL when it cannot be read;
 * the caller frees the text.
 */
static char *contents(FILE *file) {
    long  size;
    char *text;

    /* The size: seek to the end, read the position, seek back to the start. */
    if (fseek(file, 0, SEEK_END) != 0 ||
        (size = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        return NULL;
    }

    text = malloc((size_t)size + 1u);

    if (text == NULL) {
        return NULL;
    }

    if (fread(text, 1, (size_t)size, file) != (size_t)size) {
        free(text);
        return NULL;
    }

    text[size] = '\0';

    return text;
}

/**
 * A recipe as font_recipe_write writes one: the PC configuration, and the other platforms, which
 * only inherit it.
 */
static const char canonical_recipe[] =
    "MetaFileClass {\n"
    " Name \"{0123456789abcDEF}Mod/GUI/Fonts/SDF_FixtureSans32.fnt\"\n"
    " Configurations {\n"
    "  FNTResourceClass PC {\n"
    "   SourceFile \"Sources/Fixture-Regular.ttf\"\n"
    "   Characters \"../fixture.charset.txt\"\n"
    "   FontSize 32\n"
    "  }\n"
    "  FNTResourceClass XBOX_ONE : PC {\n"
    "  }\n"
    "  FNTResourceClass PS4 : PC {\n"
    "  }\n"
    "  FNTResourceClass LINUX : PC {\n"
    "  }\n"
    " }\n"
    "}\n";

/**
 * Parses the canonical recipe and writes it back: every field is read as it was written, and the
 * text written out is the text read.
 */
static int a_recipe_reads_back_exactly_what_was_written(void) {
    FILE       *input  = text_file(canonical_recipe);
    FILE       *output = temporary();
    font_recipe recipe;
    edds_error  error;
    char       *written;

    CHECK(input != NULL && output != NULL);
    CHECK(font_recipe_parse(input, &recipe, &error) == EDDS_OK);

    /* The GUID is carried character for character, case and all. */
    CHECK(strcmp(recipe.guid, "0123456789abcDEF") == 0);
    CHECK(strcmp(recipe.name, "Mod/GUI/Fonts/SDF_FixtureSans32.fnt") == 0);
    CHECK(strcmp(recipe.source_file, "Sources/Fixture-Regular.ttf") == 0);
    CHECK(strcmp(recipe.characters, "../fixture.charset.txt") == 0);
    CHECK(recipe.font_size == 32);

    /* Written back, it is the same text. */
    CHECK(font_recipe_write(output, &recipe, &error) == EDDS_OK);
    written = contents(output);
    CHECK(written != NULL && strcmp(written, canonical_recipe) == 0);

    free(written);
    fclose(input);
    fclose(output);

    return 1;
}

/**
 * Parses a recipe that also holds an author, a change date, a comment, a field the generator does
 * not know, a nested block and a FontSize for a console: written back, it is the canonical recipe.
 */
static int a_rewritten_recipe_drops_what_it_does_not_own(void) {
    FILE *input = text_file(
        "MetaFileClass {\n"
        " Name \"{0123456789abcDEF}Mod/GUI/Fonts/SDF_FixtureSans32.fnt\"\n"
        " Author \"someone\"\n"
        " ChangeDate -603265858\n"
        " Configurations {\n"
        "  // Workbench would never write this, a person might.\n"
        "  FNTResourceClass PC {\n"
        "   Characters \"../fixture.charset.txt\"\n"
        "   Shadow 2 /* a field this generator does not know */\n"
        "   SourceFile \"Sources/Fixture-Regular.ttf\"\n"
        "   Nested { Anything 1 }\n"
        "  }\n"
        "  FNTResourceClass XBOX_ONE : PC {\n"
        "   FontSize 99\n"
        "  }\n"
        " }\n"
        "}\n");
    FILE       *output = temporary();
    font_recipe recipe;
    edds_error  error;
    char       *written;

    CHECK(input != NULL && output != NULL);
    CHECK(font_recipe_parse(input, &recipe, &error) == EDDS_OK);

    /* No FontSize in PC: the default, not the console override. */
    CHECK(recipe.font_size == FONT_DEFAULT_SIZE);

    /* Written back, only what the recipe owns is left. */
    CHECK(font_recipe_write(output, &recipe, &error) == EDDS_OK);
    written = contents(output);
    CHECK(written != NULL && strcmp(written, canonical_recipe) == 0);

    free(written);
    fclose(input);
    fclose(output);

    return 1;
}

/**
 * Writes a recipe built in memory that names no character file: the text has no Characters line
 * at all, and still its FontSize.
 */
static int a_recipe_without_characters_writes_none(void) {
    FILE       *output = temporary();
    font_recipe recipe;
    edds_error  error;
    char       *written;

    /* The recipe, its characters left empty. */
    memset(&recipe, 0, sizeof recipe);
    memcpy(recipe.guid, "FEDCBA9876543210", 17);
    (void)snprintf(recipe.name, sizeof recipe.name, "Mod/SDF_A24.fnt");
    (void)snprintf(recipe.source_file, sizeof recipe.source_file, "A.ttf");
    recipe.font_size = 24;

    CHECK(output != NULL);
    CHECK(font_recipe_write(output, &recipe, &error) == EDDS_OK);
    written = contents(output);
    CHECK(written != NULL &&
        strstr(written, "Characters") == NULL &&
        strstr(written, "FontSize 24\n") != NULL);

    free(written);
    fclose(output);

    return 1;
}

/**
 * Parses six broken recipes: each is refused as invalid input, with the error code that names what
 * is wrong with it.
 */
static int an_unreadable_recipe_is_refused_with_its_reason(void) {
    /* Each broken recipe, and the code it must be refused with. */
    static const struct {
        const char *text;
        const char *code;
    } cases[] = {
        { "MetaFileClass {\n Name \"{not-a-guid}Mod/A.fnt\"\n}\n", "malformed-guid" },
        /* The empty PC block Workbench writes for a font made by Font Editor: no recipe in it. */
        { "MetaFileClass {\n Name \"{393BF215C0C82078}Gui/fonts/MetronLight22.fnt\"\n Configurations {\n"
          "  FNTResourceClass PC {\n  }\n }\n}\n",
            "missing-source-file" },
        { "MetaFileClass {\n Name \"{393BF215C0C82078}A.fnt\"\n Configurations {\n"
          "  FNTResourceClass PC {\n   SourceFile \"A.ttf\"\n   FontSize big\n  }\n }\n}\n",
            "malformed-setting" },
        { "MetaFileClass {\n Name \"{393BF215C0C82078}A.fnt\"\n Configurations {\n"
          "  FNTResourceClass PC {\n   SourceFile \"A.ttf\"\n   SourceFile \"B.ttf\"\n  }\n }\n}\n",
            "duplicate-setting" },
        { "MetaFileClass {\n Name \"{393BF215C0C82078}A.fnt\"\n Configurations {\n }\n}\n",
            "missing-pc-recipe" },
        { "MetaFileClass {\n Name \"{393BF215C0C82078}A.fnt\"\n", "malformed-recipe" }
    };

    for (size_t at = 0; at < sizeof cases / sizeof cases[0]; ++at) {
        FILE       *input = text_file(cases[at].text);
        font_recipe recipe;
        edds_error  error;

        CHECK(input != NULL);
        CHECK(font_recipe_parse(input, &recipe, &error) == EDDS_INVALID_INPUT);
        CHECK(strcmp(error.code, cases[at].code) == 0);
        fclose(input);
    }

    return 1;
}

/**
 * Reads only the GUID of two recipes: one whose PC block is empty still gives its GUID, and one
 * whose GUID is a digit short is refused as malformed.
 */
static int only_the_guid_of_a_recipe_about_to_be_replaced_matters(void) {
    FILE *broken_recipe = text_file(
        "MetaFileClass {\n Name \"{393BF215C0C82078}Gui/fonts/MetronLight22.fnt\"\n Configurations {\n"
        "  FNTResourceClass PC {\n  }\n }\n}\n");
    FILE      *broken_guid = text_file("MetaFileClass {\n Name \"{393BF215C0C8207}A.fnt\"\n}\n");
    char       guid[EDDS_METADATA_GUID_BYTES];
    edds_error error;

    CHECK(broken_recipe != NULL && broken_guid != NULL);

    /* The recipe that parsing refuses for its empty PC block still gives its GUID. */
    CHECK(font_recipe_guid(broken_recipe, guid, &error) == EDDS_OK);
    CHECK(strcmp(guid, "393BF215C0C82078") == 0);

    /* A GUID of 15 digits is not one. */
    CHECK(font_recipe_guid(broken_guid, guid, &error) == EDDS_INVALID_INPUT);
    CHECK(strcmp(error.code, "malformed-guid") == 0);

    fclose(broken_recipe);
    fclose(broken_guid);

    return 1;
}

/**
 * Parses a character file with a BOM, separators and a repeated character, then a file with an
 * overlong encoding, then takes the built-in set. Expects the members sorted and each once, the
 * overlong file refused, and the built-in set to run from the space to U+045F.
 */
static int a_character_file_is_its_characters_and_nothing_between_them(void) {
    /*
     * BOM, a line break, a tab, a space, NBSP, U+0416 twice, a CR LF: A B U+0416 and NBSP remain.
     */
    static const uint8_t text[] = {
        0xEF, 0xBB, 0xBF, 'B', 'A', '\n', 0xD0, 0x96, '\t', ' ', 0xC2, 0xA0, 0xD0, 0x96, '\r', '\n'
    };
    static const uint8_t broken[] = { 'A', 0xC0, 0x80 };
    font_characters      characters;
    edds_error           error;

    CHECK(font_characters_parse(text, sizeof text, &characters, &error) == EDDS_OK);
    CHECK(characters.count == 4);
    CHECK(characters.codes[0] == 'A' &&
        characters.codes[1] == 'B' &&
        characters.codes[2] == 0xA0u &&
        characters.codes[3] == 0x416u);
    font_characters_free(&characters);

    /* An overlong encoding of NUL is not a character. */
    CHECK(font_characters_parse(broken, sizeof broken, &characters, &error) == EDDS_INVALID_INPUT);
    CHECK(strcmp(error.code, "malformed-characters") == 0);

    /* The built-in set: Basic Latin, Latin-1 and Cyrillic, from the space up to U+045F. */
    CHECK(font_characters_builtin(&characters, &error) == EDDS_OK);
    CHECK(characters.count == 95u + 96u + 96u);
    CHECK(characters.codes[0] == 0x20u && characters.codes[characters.count - 1u] == 0x45Fu);
    font_characters_free(&characters);

    return 1;
}

/**
 * Generates a font from a fixture variant at the given size, from the given set (NULL for the
 * built-in one), and expects it refused: the status and error code given, and neither an FNT nor
 * an atlas. Returns 1 when that is what happened.
 */
static int refused(font_fixture_variant variant, uint32_t size, const font_characters *characters,
    edds_status expected, const char *code) {
    test_bytes   font = font_fixture(variant);
    font_request request;
    font_output  output;
    edds_error   error;
    edds_status  status;

    request.data       = font.data;
    request.size       = font.size;
    request.characters = characters;
    request.font_size  = size;
    request.name       = "SDF_Refused";

    /* No cancellation check and no progress report. */
    status = font_generate(&request, &output, NULL, NULL, NULL, NULL, &error);
    fixture_free(font);

    if (status != expected || strcmp(error.code, code) != 0) {
        fprintf(stderr, "expected %s, got %d %s: %s\n", code, (int)status, error.code,
            error.message);
        return 0;
    }

    return output.fnt == NULL && output.atlas == NULL;
}

/**
 * Generates from the CFF and the variable font, at sizes 7 and 41, and from all the characters of
 * the crowded font at size 40: each is refused as unsupported, with the code that names why.
 */
static int fonts_the_engine_cannot_draw_are_refused_with_their_reason(void) {
    font_characters crowded;
    uint32_t       *codes = malloc(FONT_FIXTURE_CROWDED_COUNT * sizeof *codes);

    CHECK(codes != NULL);

    /* Every character of the crowded font. */
    for (uint32_t at = 0; at < FONT_FIXTURE_CROWDED_COUNT; ++at) {
        codes[at] = FONT_FIXTURE_CROWDED_FIRST + at;
    }

    crowded.codes = codes;
    crowded.count = FONT_FIXTURE_CROWDED_COUNT;

    /* CFF outlines, a variable font, a size on either side of the range, too many cells. */
    CHECK(refused(FONT_FIXTURE_CFF, 32, NULL, EDDS_UNSUPPORTED_FORMAT,
        "unsupported-outline-format"));
    CHECK(refused(FONT_FIXTURE_VARIABLE, 32, NULL, EDDS_UNSUPPORTED_FORMAT,
        "variable-font-unsupported"));
    CHECK(refused(FONT_FIXTURE_GPOS, 7, NULL, EDDS_UNSUPPORTED_FORMAT, "font-size-out-of-range"));
    CHECK(refused(FONT_FIXTURE_GPOS, 41, NULL, EDDS_UNSUPPORTED_FORMAT, "font-size-out-of-range"));
    CHECK(refused(FONT_FIXTURE_CROWDED, 40, &crowded, EDDS_UNSUPPORTED_FORMAT, "atlas-too-large"));

    free(codes);

    return 1;
}

/**
 * Generates the GPOS font from every character it maps plus U+0416, which it lacks: U+0416 is
 * reported missing and U+25A1 drawn, the atlas is opaque and a power of two each way, and the FNT
 * inspects back with the name, the size and the counts of the output.
 */
static int a_font_carries_what_it_has_and_names_what_it_lacks(void) {
    /* The GPOS font, and the characters it maps. */
    const uint32_t *codes = NULL;
    const size_t    count = font_fixture_codes(FONT_FIXTURE_GPOS, &codes);
    test_bytes      font  = font_fixture(FONT_FIXTURE_GPOS);

    /* The set asked for. */
    uint32_t        wanted_codes[64];
    font_characters wanted;

    /* The generation, and its FNT read back from a file. */
    font_request request;
    font_output  output;
    edds_error   error;
    FILE        *fnt = temporary();
    font_info    info;

    /* How many characters the set holds so far. */
    size_t wanted_count = 0;

    CHECK(font.data != NULL && fnt != NULL);

    /* Every character the font maps, then U+0416, which it lacks. */
    for (size_t at = 0; at < count; ++at) {
        wanted_codes[wanted_count++] = codes[at];
    }

    wanted_codes[wanted_count++] = 0x0416u;

    wanted.codes       = wanted_codes;
    wanted.count       = wanted_count;
    request.data       = font.data;
    request.size       = font.size;
    request.characters = &wanted;
    request.font_size  = 32;
    request.name       = "SDF_FixtureSans32";

    CHECK(font_generate(&request, &output, NULL, NULL, NULL, NULL, &error) == EDDS_OK);
    CHECK(output.missing_count == 1 && output.missing[0] == 0x0416u);
    CHECK(output.drawn_count == 1 && output.drawn[0] == FONT_MISSING_BOX);

    /* Every code the font maps, plus the box it had to have drawn. */
    CHECK(output.glyph_count == count + 1u);

    /* The names of the source font. */
    CHECK(strcmp(output.source.family, "Fixture Sans") == 0 &&
        strcmp(output.source.style, "Regular") == 0);

    /* The atlas: at least as wide as high, within the limit, a power of two each way, opaque. */
    CHECK(output.atlas_width >= output.atlas_height && output.atlas_width <= FONT_MAX_ATLAS);
    CHECK((output.atlas_width & (output.atlas_width - 1u)) == 0 &&
        (output.atlas_height & (output.atlas_height - 1u)) == 0);

    for (size_t at = 3; at < (size_t)output.atlas_width * output.atlas_height * 4u; at += 4u) {
        CHECK(output.atlas[at] == 255u);
    }

    /* The FNT, written to a file and inspected: its header, and the counts the output gave. */
    CHECK(fwrite(output.fnt, 1, output.fnt_size, fnt) == output.fnt_size &&
        fseek(fnt, 0, SEEK_SET) == 0);
    CHECK(font_inspect(fnt, &info, &error) == EDDS_OK);
    CHECK(strcmp(info.name, "SDF_FixtureSans32") == 0 &&
        info.size == 32 &&
        info.type == 2 &&
        info.r == 8);
    CHECK(info.glyph_count == output.glyph_count && info.pair_count == output.pair_count);
    CHECK((uint32_t)info.cell == output.cell && info.range_count == output.range_count);

    font_info_free(&info);
    font_output_free(&output);
    fixture_free(font);
    fclose(fnt);

    return 1;
}

/**
 * Generates the KERN font, which has no cap height, from A and V only: the FNT still takes its cap
 * height from the H the set leaves out.
 */
static int a_font_without_a_cap_height_measures_its_h_even_when_the_set_leaves_h_out(void) {
    /* A set of A and V, without H. */
    uint32_t              no_h[] = { 'A', 'V' };
    const font_characters wanted = { no_h, 2 };

    /* The KERN font, its generation, and its FNT read back from a file. */
    test_bytes   font = font_fixture(FONT_FIXTURE_KERN);
    font_request request;
    font_output  output;
    edds_error   error;
    FILE        *fnt = temporary();
    font_info    info;

    CHECK(font.data != NULL && fnt != NULL);

    request.data       = font.data;
    request.size       = font.size;
    request.characters = &wanted;
    request.font_size  = 32;
    request.name       = "SDF_Fixture32";

    CHECK(font_generate(&request, &output, NULL, NULL, NULL, NULL, &error) == EDDS_OK);
    CHECK(fwrite(output.fnt, 1, output.fnt_size, fnt) == output.fnt_size &&
        fseek(fnt, 0, SEEK_SET) == 0);
    CHECK(font_inspect(fnt, &info, &error) == EDDS_OK);

    /*
     * The kern variant has no OS/2 cap height and an H 720 units tall: 23.04 px at 32, not 0.7 em.
     */
    CHECK(info.cap_height == 23.0f);

    font_info_free(&info);
    font_output_free(&output);
    fixture_free(font);
    fclose(fnt);

    return 1;
}

/**
 * Generates a font, writes a float that is not a number over the cap height in its FNT header, and
 * expects inspection to refuse the FNT as malformed.
 */
static int an_fnt_whose_header_metrics_are_not_numbers_is_refused(void) {
    /* The name of the FNT, where its cap height A lies, and what is written over it. */
    static const char    name[]          = "SDF_Fixture32";
    /* FORM, size, FNT5, HEAD, size, name length, the name, size, zero, type and cell: then A. */
    const size_t         cap_height_at   = 24u + sizeof name + 13u;
    static const uint8_t not_a_number[4] = { 0x00, 0x00, 0xC0, 0x7F };

    /* The GPOS font, its generation, and its FNT read back from a file. */
    test_bytes   font = font_fixture(FONT_FIXTURE_GPOS);
    font_request request;
    font_output  output;
    edds_error   error;
    FILE        *fnt = temporary();
    font_info    info;

    CHECK(font.data != NULL && fnt != NULL);

    /* The built-in set, at size 32. */
    request.data       = font.data;
    request.size       = font.size;
    request.characters = NULL;
    request.font_size  = 32;
    request.name       = name;

    CHECK(font_generate(&request, &output, NULL, NULL, NULL, NULL, &error) == EDDS_OK);
    CHECK(output.fnt_size > cap_height_at + 4u);

    /* A overwritten, then the FNT written to a file and inspected. */
    memcpy(output.fnt + cap_height_at, not_a_number, sizeof not_a_number);
    CHECK(fwrite(output.fnt, 1, output.fnt_size, fnt) == output.fnt_size &&
        fseek(fnt, 0, SEEK_SET) == 0);
    CHECK(font_inspect(fnt, &info, &error) == EDDS_INVALID_INPUT);
    CHECK(strcmp(error.code, "malformed-fnt") == 0);

    font_output_free(&output);
    fixture_free(font);
    fclose(fnt);

    return 1;
}

/**
 * Describes the KERN font, whose name table holds a typographic family and style (Fixture,
 * Medium) beside its family and style (Fixture Medium, Regular): the typographic ones are the ones
 * given, with the units per em.
 */
static int a_source_says_its_typographic_names_first(void) {
    test_bytes       font = font_fixture(FONT_FIXTURE_KERN);
    font_source_info info;
    edds_error       error;

    CHECK(font.data != NULL);
    CHECK(font_source_describe(font.data, font.size, &info, &error) == EDDS_OK);
    CHECK(strcmp(info.family, "Fixture") == 0 && strcmp(info.style, "Medium") == 0);
    CHECK(info.units_per_em == FONT_FIXTURE_UNITS_PER_EM);

    fixture_free(font);

    return 1;
}

/** Runs the tests in order, stopping at the first that fails. Exits 0 when all pass, 1 if not. */
int main(void) {
    const int passed =
        a_recipe_reads_back_exactly_what_was_written() &&
        a_rewritten_recipe_drops_what_it_does_not_own() &&
        a_recipe_without_characters_writes_none() &&
        an_unreadable_recipe_is_refused_with_its_reason() &&
        only_the_guid_of_a_recipe_about_to_be_replaced_matters() &&
        a_character_file_is_its_characters_and_nothing_between_them() &&
        fonts_the_engine_cannot_draw_are_refused_with_their_reason() &&
        a_font_carries_what_it_has_and_names_what_it_lacks() &&
        a_font_without_a_cap_height_measures_its_h_even_when_the_set_leaves_h_out() &&
        an_fnt_whose_header_metrics_are_not_numbers_is_refused() &&
        a_source_says_its_typographic_names_first();

    return passed ? 0 : 1;
}
