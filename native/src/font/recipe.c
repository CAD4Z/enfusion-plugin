#include "font_internal.h"
#include "meta_text.h"

#include <stdarg.h>
#include <string.h>

void font_fail(edds_error *error, const char *code, const char *format, ...) {
    va_list arguments;
    if (error == NULL) return;
    memset(error, 0, sizeof *error);
    (void)snprintf(error->code, sizeof error->code, "%s", code);
    va_start(arguments, format);
    (void)vsnprintf(error->message, sizeof error->message, format, arguments);
    va_end(arguments);
}

enum {
    SETTING_SOURCE = 1u << 0,
    SETTING_CHARACTERS = 1u << 1,
    SETTING_SIZE = 1u << 2
};

static edds_status malformed(edds_error *error, const char *what) {
    font_fail(error, "malformed-recipe", "The font recipe is malformed: %s.", what);
    return EDDS_INVALID_INPUT;
}

static int size_value(const char *text, uint32_t *value) {
    uint32_t parsed = 0;
    if (*text == '\0') return 0;
    for (; *text != '\0'; ++text) {
        if (*text < '0' || *text > '9' || parsed > 100000u) return 0;
        parsed = parsed * 10u + (uint32_t)(*text - '0');
    }
    *value = parsed;
    return 1;
}

static edds_status parse_pc(meta_scanner *scan, font_recipe *recipe, edds_error *error) {
    uint32_t seen = 0;
    for (;;) {
        const meta_token key = meta_next_token(scan);
        meta_token value;
        uint32_t bit = 0;
        if (key.kind == META_TOKEN_CLOSE) break;
        if (key.kind != META_TOKEN_WORD) return malformed(error, "the PC configuration");
        value = meta_next_token(scan);
        if (value.kind == META_TOKEN_OPEN) {
            if (!meta_skip_open_block(scan)) return malformed(error, "a block is not closed");
            continue;
        }
        if (strcmp(key.text, "SourceFile") == 0) bit = SETTING_SOURCE;
        else if (strcmp(key.text, "Characters") == 0) bit = SETTING_CHARACTERS;
        else if (strcmp(key.text, "FontSize") == 0) bit = SETTING_SIZE;
        else continue;
        if ((seen & bit) != 0) {
            font_fail(error, "duplicate-setting", "Font recipe setting %s occurs more than once in PC.", key.text);
            return EDDS_INVALID_INPUT;
        }
        seen |= bit;
        if (bit == SETTING_SIZE) {
            if (value.kind != META_TOKEN_WORD || !size_value(value.text, &recipe->font_size)) {
                font_fail(error, "malformed-setting", "Font recipe setting FontSize must be a whole number.");
                return EDDS_INVALID_INPUT;
            }
            continue;
        }
        if (value.kind != META_TOKEN_STRING ||
            !meta_copy_text(bit == SETTING_SOURCE ? recipe->source_file : recipe->characters,
                EDDS_METADATA_PATH_BYTES, value.text)) {
            font_fail(error, "malformed-setting", "Font recipe setting %s must be a quoted path.", key.text);
            return EDDS_INVALID_INPUT;
        }
    }
    if ((seen & SETTING_SOURCE) == 0 || recipe->source_file[0] == '\0') {
        font_fail(error, "missing-source-file", "The font recipe does not name a SourceFile in PC.");
        return EDDS_INVALID_INPUT;
    }
    return EDDS_OK;
}

static edds_status parse_configurations(meta_scanner *scan, font_recipe *recipe, edds_error *error) {
    int found_pc = 0;
    for (;;) {
        const meta_token resource = meta_next_token(scan);
        meta_token platform;
        meta_token next;
        if (resource.kind == META_TOKEN_CLOSE) break;
        if (resource.kind != META_TOKEN_WORD) return malformed(error, "the Configurations block");
        platform = meta_next_token(scan);
        if (platform.kind != META_TOKEN_WORD) return malformed(error, "the Configurations block");
        next = meta_next_token(scan);
        if (next.kind == META_TOKEN_COLON) {
            if (meta_next_token(scan).kind != META_TOKEN_WORD) return malformed(error, "a platform parent");
            next = meta_next_token(scan);
        }
        if (next.kind != META_TOKEN_OPEN) return malformed(error, "the Configurations block");
        if (strcmp(resource.text, "FNTResourceClass") == 0 && strcmp(platform.text, "PC") == 0) {
            edds_status status;
            if (found_pc) {
                font_fail(error, "duplicate-pc-recipe", "The font recipe has more than one PC configuration.");
                return EDDS_INVALID_INPUT;
            }
            found_pc = 1;
            status = parse_pc(scan, recipe, error);
            if (status != EDDS_OK) return status;
        } else if (!meta_skip_open_block(scan)) {
            return malformed(error, "a configuration is not closed");
        }
    }
    if (!found_pc) {
        font_fail(error, "missing-pc-recipe", "The font recipe has no FNTResourceClass PC configuration.");
        return EDDS_INVALID_INPUT;
    }
    return EDDS_OK;
}

/**
 * Walks the MetaFileClass block. With `recipe` NULL it stops at a valid Name and reads nothing
 * else, which is all a recipe that is about to be replaced has to give.
 */
static edds_status parse(FILE *input, font_recipe *recipe, char guid[EDDS_METADATA_GUID_BYTES], edds_error *error) {
    char *source = NULL;
    size_t size = 0;
    meta_scanner scan;
    meta_token value;
    int found_name = 0;
    int found_configurations = 0;
    char name[EDDS_METADATA_PATH_BYTES];
    edds_status status = EDDS_INVALID_INPUT;
    if (!meta_read_text(input, &source, &size, error)) return EDDS_INVALID_INPUT;
    scan.source = source;
    scan.size = size;
    scan.at = 0;
    value = meta_next_token(&scan);
    if (value.kind != META_TOKEN_WORD || strcmp(value.text, "MetaFileClass") != 0 ||
        meta_next_token(&scan).kind != META_TOKEN_OPEN) {
        status = malformed(error, "it must be one MetaFileClass block");
        goto done;
    }
    for (;;) {
        const meta_token key = meta_next_token(&scan);
        meta_token field;
        if (key.kind == META_TOKEN_CLOSE) break;
        if (key.kind != META_TOKEN_WORD) {
            status = malformed(error, "the MetaFileClass block");
            goto done;
        }
        field = meta_next_token(&scan);
        if (strcmp(key.text, "Name") == 0) {
            if (found_name || field.kind != META_TOKEN_STRING ||
                !meta_parse_name(field.text, guid, name, sizeof name)) {
                font_fail(error, "malformed-guid", "The recipe Name must begin with one 64-bit hexadecimal GUID.");
                goto done;
            }
            found_name = 1;
            if (recipe == NULL) {
                status = EDDS_OK;
                goto done;
            }
            (void)meta_copy_text(recipe->name, sizeof recipe->name, name);
        } else if (strcmp(key.text, "Configurations") == 0 && recipe != NULL) {
            if (found_configurations || field.kind != META_TOKEN_OPEN) {
                status = malformed(error, "it must hold one Configurations block");
                goto done;
            }
            found_configurations = 1;
            status = parse_configurations(&scan, recipe, error);
            if (status != EDDS_OK) goto done;
            status = EDDS_INVALID_INPUT;
        } else if (field.kind == META_TOKEN_OPEN && !meta_skip_open_block(&scan)) {
            status = malformed(error, "a block is not closed");
            goto done;
        } else if (field.kind == META_TOKEN_INVALID || field.kind == META_TOKEN_END) {
            status = malformed(error, "a field has no value");
            goto done;
        }
    }
    if (!found_name) {
        font_fail(error, "malformed-guid", "The recipe has no Name with a GUID.");
        goto done;
    }
    if (meta_next_token(&scan).kind != META_TOKEN_END || !found_configurations) {
        status = malformed(error, "it must hold Name and one Configurations block, and nothing after");
        goto done;
    }
    status = EDDS_OK;

done:
    edds_free(source);
    return status;
}

edds_status font_recipe_parse(FILE *input, font_recipe *recipe, edds_error *error) {
    if (input == NULL || recipe == NULL) {
        font_fail(error, "invalid-api-argument", "The recipe input and value are required.");
        return EDDS_INTERNAL_FAILURE;
    }
    memset(recipe, 0, sizeof *recipe);
    recipe->font_size = FONT_DEFAULT_SIZE;
    return parse(input, recipe, recipe->guid, error);
}

edds_status font_recipe_guid(FILE *input, char guid[EDDS_METADATA_GUID_BYTES], edds_error *error) {
    if (input == NULL || guid == NULL) {
        font_fail(error, "invalid-api-argument", "The recipe input and GUID are required.");
        return EDDS_INTERNAL_FAILURE;
    }
    memset(guid, 0, EDDS_METADATA_GUID_BYTES);
    return parse(input, NULL, guid, error);
}

static int quotable(const char *value, int may_be_empty) {
    if (*value == '\0') return may_be_empty;
    for (; *value != '\0'; ++value) {
        if (*value == '"' || *value == '\\' || (unsigned char)*value < 0x20u) return 0;
    }
    return 1;
}

edds_status font_recipe_write(FILE *output, const font_recipe *recipe, edds_error *error) {
    int written;
    if (output == NULL || recipe == NULL) {
        font_fail(error, "invalid-api-argument", "The recipe value and output are required.");
        return EDDS_INTERNAL_FAILURE;
    }
    if (!meta_valid_guid(recipe->guid)) {
        font_fail(error, "malformed-guid", "The recipe GUID must be exactly 16 hexadecimal characters.");
        return EDDS_INVALID_INPUT;
    }
    if (!quotable(recipe->name, 0) || !quotable(recipe->source_file, 0) ||
        !quotable(recipe->characters, 1)) {
        font_fail(error, "invalid-recipe-value", "Recipe names and paths must be non-empty text without quotes.");
        return EDDS_INVALID_INPUT;
    }
    written = fprintf(output,
        "MetaFileClass {\n"
        " Name \"{%s}%s\"\n"
        " Configurations {\n"
        "  FNTResourceClass PC {\n"
        "   SourceFile \"%s\"\n",
        recipe->guid, recipe->name, recipe->source_file);
    if (written >= 0 && recipe->characters[0] != '\0') {
        written = fprintf(output, "   Characters \"%s\"\n", recipe->characters);
    }
    if (written >= 0) {
        written = fprintf(output,
            "   FontSize %u\n"
            "  }\n"
            "  FNTResourceClass XBOX_ONE : PC {\n"
            "  }\n"
            "  FNTResourceClass PS4 : PC {\n"
            "  }\n"
            "  FNTResourceClass LINUX : PC {\n"
            "  }\n"
            " }\n"
            "}\n",
            recipe->font_size);
    }
    if (written < 0 || fflush(output) != 0) {
        font_fail(error, "recipe-write-failed", "The font recipe could not be written completely.");
        return EDDS_INTERNAL_FAILURE;
    }
    return EDDS_OK;
}
