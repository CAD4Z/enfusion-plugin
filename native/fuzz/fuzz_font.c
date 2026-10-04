#include <font/font.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Everything a TrueType file reaches: the table directory, the character map, every outline the
 * set maps to with its components, the union of overlapping contours, the field, and the kerning
 * lookups. The smallest size and a short set keep one input cheap; the corpus decides which glyph
 * each of these codes lands on.
 */
static uint32_t codes[] = { 0x20, 0x41, 0x48, 0x4F, 0x54, 0x56, 0x61, 0x6F, 0xC4, 0xC5, 0x1DE, 0x426, 0x2C6F, 0x1F600 };

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    font_characters set = { codes, sizeof codes / sizeof codes[0] };
    font_characters parsed;
    font_source_info info;
    font_request request;
    font_output output;
    font_recipe recipe;
    font_info inspected;
    char guid[EDDS_METADATA_GUID_BYTES];
    edds_error error;
    FILE *file;

    (void)font_source_describe(data, size, &info, &error);
    request.data = data;
    request.size = size;
    request.characters = &set;
    request.font_size = FONT_MIN_SIZE;
    request.name = "SDF_Fuzz";
    if (font_generate(&request, &output, NULL, NULL, NULL, NULL, &error) == EDDS_OK) {
        font_output_free(&output);
    }

    /* The same bytes as a character set, a recipe and an FNT file. */
    if (font_characters_parse(data, size, &parsed, &error) == EDDS_OK) {
        font_characters_free(&parsed);
    }
    file = tmpfile();
    if (file == NULL) {
        return 0;
    }
    if (size == 0 || fwrite(data, 1, size, file) == size) {
        rewind(file);
        (void)font_recipe_parse(file, &recipe, &error);
        rewind(file);
        (void)font_recipe_guid(file, guid, &error);
        rewind(file);
        if (font_inspect(file, &inspected, &error) == EDDS_OK) {
            font_info_free(&inspected);
        }
    }
    fclose(file);
    return 0;
}
