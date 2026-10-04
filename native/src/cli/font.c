#ifdef _WIN32
#define _CRT_RAND_S
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include "cli.h"
#include "meta_text.h"

#include <font/font.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <dirent.h>
#include <limits.h>
#endif

/*
 * `enfusion font generate | inspect`. A font is three files beside each other — the recipe
 * `.fnt.meta`, the `.fnt` and its `.edds` atlas — and they are replaced together or not at all.
 */

enum {
    FONT_PROTOCOL_VERSION = 1
};

typedef struct font_options {
    int             machine;
    int             protocol_seen;
    uint32_t        protocol;
    const cli_char *meta;
    const cli_char *input;
    const cli_char *output;
    const cli_char *resource_name;
    int             size_seen;
    uint32_t        size;
    const cli_char *characters;
    const cli_char *guid;
    const cli_char *cancel_file;
} font_options;

static void usage(void) {
    fputs(
        "usage:\n"
        "  enfusion font generate --machine --protocol 1 --meta PATH.fnt.meta [--resource-name NAME]\n"
        "  enfusion font generate --machine --protocol 1 --input SOURCE.ttf --output FONT.fnt\n"
        "      --resource-name NAME [--size N] [--characters FILE] [--guid HEX]\n"
        "  enfusion font inspect --machine --protocol 1 --input FONT.fnt|SOURCE.ttf\n",
        stderr);
}

static edds_status refuse(edds_error *error, edds_status status, const char *code, const char *message) {
    memset(error, 0, sizeof *error);
    (void)snprintf(error->code, sizeof error->code, "%s", code);
    (void)snprintf(error->message, sizeof error->message, "%s", message);
    return status;
}

static int invalid(const char *code, const char *message) {
    edds_error error;
    usage();
    return report_failure(refuse(&error, EDDS_INVALID_INVOCATION, code, message), &error);
}

static int parse_options(int argc, cli_char **argv, font_options *options) {
    memset(options, 0, sizeof *options);
    for (int at = 2; at < argc; ++at) {
        const cli_char **path = NULL;
        if (equals(argv[at], "--machine") && !options->machine) {
            options->machine = 1;
            continue;
        }
        if (at + 1 >= argc) {
            return 0;
        }
        if (equals(argv[at], "--protocol") && !options->protocol_seen) {
            options->protocol_seen = unsigned_argument(argv[++at], &options->protocol);
            if (!options->protocol_seen) {
                return 0;
            }
            continue;
        }
        if (equals(argv[at], "--size") && !options->size_seen) {
            options->size_seen = unsigned_argument(argv[++at], &options->size);
            if (!options->size_seen) {
                return 0;
            }
            continue;
        }
        if (equals(argv[at], "--meta")) {
            path = &options->meta;
        } else if (equals(argv[at], "--input")) {
            path = &options->input;
        } else if (equals(argv[at], "--output")) {
            path = &options->output;
        } else if (equals(argv[at], "--resource-name")) {
            path = &options->resource_name;
        } else if (equals(argv[at], "--characters")) {
            path = &options->characters;
        } else if (equals(argv[at], "--guid")) {
            path = &options->guid;
        } else if (equals(argv[at], "--cancel-file")) {
            path = &options->cancel_file;
        }
        if (path == NULL || *path != NULL) {
            return 0;
        }
        *path = argv[++at];
    }
    return options->machine && options->protocol_seen && options->protocol == FONT_PROTOCOL_VERSION;
}

/** A copy of `path` without its last `cut` characters, with `suffix` appended. */
static cli_char *replace_tail(const cli_char *path, size_t cut, const char *suffix) {
    const size_t keep = cli_strlen(path) - cut, extra = strlen(suffix);
    cli_char    *result = malloc((keep + extra + 1u) * sizeof *result);
    if (result == NULL) {
        return NULL;
    }
    memcpy(result, path, keep * sizeof *result);
    for (size_t at = 0; at < extra; ++at) {
        result[keep + at] = (cli_char)(unsigned char)suffix[at];
    }
    result[keep + extra] = 0;
    return result;
}

/**
 * The folder part of a path, `.` when it has none, never with a trailing separator — except the
 * one a root is made of: `D:` alone is the current folder on D, where `D:\` is its root.
 */
static cli_char *folder_of(const cli_char *path) {
    size_t end = cli_strlen(path);
    while (end > 0 && !is_separator(path[end - 1u])) {
        --end;
    }
    if (end == 0) {
        return replace_tail(path, cli_strlen(path), ".");
    }
    if (end > 1u && !(end == 3u && path[1] == (cli_char)':')) {
        --end;
    }
    return replace_tail(path, cli_strlen(path) - end, "");
}

/** The file name without its last extension, as UTF-8: what the FNT header calls the font. */
static char *stem_of(const cli_char *path) {
    size_t start = cli_strlen(path), end;
    char  *name;
    while (start > 0 && !is_separator(path[start - 1u])) {
        --start;
    }
    end = cli_strlen(path);
    while (end > start && path[end - 1u] != (cli_char)'.') {
        --end;
    }
    if (end > start) {
        --end;
    } else {
        end = cli_strlen(path);
    }
    {
        cli_char *copy = malloc((end - start + 1u) * sizeof *copy);
        if (copy == NULL) {
            return NULL;
        }
        memcpy(copy, path + start, (end - start) * sizeof *copy);
        copy[end - start] = 0;
        name              = utf8_of(copy);
        free(copy);
    }
    return name;
}

static int read_file(const cli_char *path, uint64_t limit, uint8_t **data, size_t *size) {
    FILE *file = open_input(path);
    long  length;
    *data = NULL;
    *size = 0;
    if (file == NULL) {
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0 ||
        (uint64_t)length > limit) {
        (void)fclose(file);
        return -1;
    }
    *data = malloc((size_t)length == 0 ? 1u : (size_t)length);
    if (*data == NULL || fread(*data, 1, (size_t)length, file) != (size_t)length) {
        free(*data);
        *data = NULL;
        (void)fclose(file);
        return 0;
    }
    (void)fclose(file);
    *size = (size_t)length;
    return 1;
}

/* --- Recipe paths ----------------------------------------------------------------------------- */

static int absolute_recipe_path(const char *path) {
    return path[0] == '/' || path[0] == '\\' || (path[0] != '\0' && path[1] == ':');
}

/** `folder/relative`, for a recipe path read relative to the folder of its recipe. */
static cli_char *joined(const cli_char *folder, const char *relative) {
    cli_char *tail = cli_of_utf8(relative);
    cli_char *result;
    size_t    folder_size, tail_size;
    if (tail == NULL) {
        return NULL;
    }
    folder_size = cli_strlen(folder);
    tail_size   = cli_strlen(tail);
    result      = malloc((folder_size + tail_size + 2u) * sizeof *result);
    if (result != NULL) {
        memcpy(result, folder, folder_size * sizeof *result);
        result[folder_size] = (cli_char)'/';
        memcpy(result + folder_size + 1u, tail, (tail_size + 1u) * sizeof *result);
    }
    free(tail);
    return result;
}

static cli_char *full_path(const cli_char *path) {
#ifdef _WIN32
    const DWORD needed = GetFullPathNameW(path, 0, NULL, NULL);
    cli_char   *result;
    if (needed == 0) {
        return NULL;
    }
    result = malloc((size_t)needed * sizeof *result);
    if (result == NULL) {
        return NULL;
    }
    if (GetFullPathNameW(path, needed, result, NULL) >= needed) {
        free(result);
        return NULL;
    }
    return result;
#else
    char       *folder = folder_of(path);
    const char *name   = path + strlen(path);
    char        resolved[PATH_MAX];
    char       *result;
    while (name > path && name[-1] != '/') {
        --name;
    }
    if (folder == NULL || realpath(folder, resolved) == NULL) {
        free(folder);
        return NULL;
    }
    free(folder);
    result = malloc(strlen(resolved) + strlen(name) + 2u);
    if (result != NULL) {
        (void)snprintf(result, strlen(resolved) + strlen(name) + 2u, "%s/%s", resolved, name);
    }
    return result;
#endif
}

typedef struct segment {
    const cli_char *start;
    size_t          size;
} segment;

/** The named folders of an absolute path, `.` dropped and `..` walked back; NULL on failure. */
static segment *segments_of(const cli_char *path, size_t *count) {
    const size_t length = cli_strlen(path);
    segment     *list   = malloc((length / 2u + 2u) * sizeof *list);
    size_t       at     = 0;
    *count              = 0;
    if (list == NULL) {
        return NULL;
    }
    while (at < length) {
        const size_t start = at;
        while (at < length && !is_separator(path[at])) {
            ++at;
        }
        if (at - start == 1u && path[start] == (cli_char)'.') {
            /* Here: nothing to record. */
        } else if (at - start == 2u && path[start] == (cli_char)'.' && path[start + 1u] == (cli_char)'.') {
            if (*count > 1u) {
                --*count;
            }
        } else if (at > start) {
            list[*count].start = path + start;
            list[*count].size  = at - start;
            ++*count;
        }
        while (at < length && is_separator(path[at])) {
            ++at;
        }
    }
    return list;
}

static int same_segment(segment a, segment b) {
    if (a.size != b.size) {
        return 0;
    }
    for (size_t at = 0; at < a.size; ++at) {
#ifdef _WIN32
        if (ascii_lower(a.start[at]) != ascii_lower(b.start[at])) {
            return 0;
        }
#else
        if (a.start[at] != b.start[at]) {
            return 0;
        }
#endif
    }
    return 1;
}

/**
 * `target` as the recipe holds it: relative to `folder`, forward slashes, `..` where it climbs.
 * NULL when nothing relative leads there, such as another drive.
 */
static char *relative_to(const cli_char *folder, const cli_char *target) {
    cli_char *from = full_path(folder), *to = full_path(target);
    segment  *from_segments = NULL, *to_segments = NULL;
    size_t    from_count = 0, to_count = 0, common = 0, size = 1;
    cli_char *relative = NULL;
    char     *result   = NULL;
    if (from == NULL || to == NULL) {
        goto done;
    }
    from_segments = segments_of(from, &from_count);
    to_segments   = segments_of(to, &to_count);
    if (from_segments == NULL || to_segments == NULL || to_count == 0) {
        goto done;
    }
#ifdef _WIN32
    /* A drive or a share is a root: another one has no relative path from here. */
    if (from_count == 0 || !same_segment(from_segments[0], to_segments[0])) {
        goto done;
    }
#endif
    while (common < from_count && common + 1u < to_count && same_segment(from_segments[common], to_segments[common])) {
        ++common;
    }
    size += 3u * (from_count - common);
    for (size_t at = common; at < to_count; ++at) {
        size += to_segments[at].size + 1u;
    }
    relative = malloc(size * sizeof *relative);
    if (relative == NULL) {
        goto done;
    }
    {
        size_t out = 0;
        for (size_t climb = common; climb < from_count; ++climb) {
            relative[out++] = (cli_char)'.';
            relative[out++] = (cli_char)'.';
            relative[out++] = (cli_char)'/';
        }
        for (size_t at = common; at < to_count; ++at) {
            memcpy(relative + out, to_segments[at].start, to_segments[at].size * sizeof *relative);
            out += to_segments[at].size;
            if (at + 1u < to_count) {
                relative[out++] = (cli_char)'/';
            }
        }
        relative[out] = 0;
    }
    result = utf8_of(relative);

done:
    free(from);
    free(to);
    free(from_segments);
    free(to_segments);
    free(relative);
    return result;
}

/* --- Identity --------------------------------------------------------------------------------- */

static int random_bytes(uint8_t *bytes, size_t count) {
#ifdef _WIN32
    for (size_t at = 0; at < count; at += 4u) {
        unsigned int value;
        if (rand_s(&value) != 0) {
            return 0;
        }
        for (size_t byte = 0; byte < 4u && at + byte < count; ++byte) {
            bytes[at + byte] = (uint8_t)(value >> (8u * byte));
        }
    }
    return 1;
#else
    FILE *source = fopen("/dev/urandom", "rb");
    int   ok;
    if (source == NULL) {
        return 0;
    }
    ok = fread(bytes, 1, count, source) == count;
    (void)fclose(source);
    return ok;
#endif
}

static int same_guid(const char *a, const char *b) {
    for (size_t at = 0; at < 16u; ++at) {
        if (toupper((unsigned char)a[at]) != toupper((unsigned char)b[at])) {
            return 0;
        }
    }
    return 1;
}

/** Whether a `.meta` beside the font already claims this GUID. */
static int guid_taken(const cli_char *folder, const char *guid) {
    int taken = 0;
#ifdef _WIN32
    cli_char        *pattern = replace_tail(folder, 0, "\\*.meta");
    WIN32_FIND_DATAW found;
    HANDLE           search;
    if (pattern == NULL) {
        return 1;
    }
    search = FindFirstFileW(pattern, &found);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        cli_char *path = replace_tail(folder, 0, "\\");
        cli_char *full = NULL;
        if (path != NULL) {
            const size_t base = cli_strlen(path), name = cli_strlen(found.cFileName);
            full = malloc((base + name + 1u) * sizeof *full);
            if (full != NULL) {
                memcpy(full, path, base * sizeof *full);
                memcpy(full + base, found.cFileName, (name + 1u) * sizeof *full);
            }
        }
        if (full != NULL) {
            FILE      *meta = open_input(full);
            char       existing[EDDS_METADATA_GUID_BYTES];
            edds_error ignored;
            if (meta != NULL) {
                if (font_recipe_guid(meta, existing, &ignored) == EDDS_OK && same_guid(existing, guid)) {
                    taken = 1;
                }
                (void)fclose(meta);
            }
        }
        free(path);
        free(full);
    } while (!taken && FindNextFileW(search, &found));
    FindClose(search);
#else
    DIR           *directory = opendir(folder);
    struct dirent *entry;
    if (directory == NULL) {
        return 0;
    }
    while (!taken && (entry = readdir(directory)) != NULL) {
        const size_t length = strlen(entry->d_name);
        char        *full;
        if (length < 5u || strcmp(entry->d_name + length - 5u, ".meta") != 0) {
            continue;
        }
        full = malloc(strlen(folder) + length + 2u);
        if (full == NULL) {
            continue;
        }
        (void)snprintf(full, strlen(folder) + length + 2u, "%s/%s", folder, entry->d_name);
        {
            FILE      *meta = fopen(full, "rb");
            char       existing[EDDS_METADATA_GUID_BYTES];
            edds_error ignored;
            if (meta != NULL) {
                if (font_recipe_guid(meta, existing, &ignored) == EDDS_OK && same_guid(existing, guid)) {
                    taken = 1;
                }
                (void)fclose(meta);
            }
        }
        free(full);
    }
    (void)closedir(directory);
#endif
    return taken;
}

static edds_status new_guid(const cli_char *folder, char guid[EDDS_METADATA_GUID_BYTES], edds_error *error) {
    static const char digits[] = "0123456789ABCDEF";
    for (int attempt = 0; attempt < 16; ++attempt) {
        uint8_t bytes[8];
        if (!random_bytes(bytes, sizeof bytes)) {
            break;
        }
        for (size_t at = 0; at < 8u; ++at) {
            guid[2u * at]      = digits[bytes[at] >> 4];
            guid[2u * at + 1u] = digits[bytes[at] & 15u];
        }
        guid[16] = '\0';
        if (!guid_taken(folder, guid)) {
            return EDDS_OK;
        }
    }
    return refuse(error, EDDS_INTERNAL_FAILURE, "guid-generation-failed", "No unused GUID could be generated.");
}

/* --- Generate --------------------------------------------------------------------------------- */

typedef struct font_paths {
    cli_char *meta;
    cli_char *fnt;
    cli_char *atlas;
    cli_char *source;
    cli_char *characters;
    cli_char *folder;
} font_paths;

static void free_paths(font_paths *paths) {
    free(paths->meta);
    free(paths->fnt);
    free(paths->atlas);
    free(paths->source);
    free(paths->characters);
    free(paths->folder);
    memset(paths, 0, sizeof *paths);
}

static edds_status recipe_from_meta(const font_options *options, font_paths *paths, font_recipe *recipe,
    edds_error *error) {
    FILE       *input = open_input(options->meta);
    edds_status status;
    if (input == NULL) {
        return refuse(error, EDDS_INVALID_INPUT, "recipe-open-failed", "The font recipe could not be opened.");
    }
    status = font_recipe_parse(input, recipe, error);
    (void)fclose(input);
    if (status != EDDS_OK) {
        return status;
    }
    if (absolute_recipe_path(recipe->source_file) || absolute_recipe_path(recipe->characters)) {
        return refuse(error, EDDS_INVALID_INPUT, "recipe-path-not-relative",
            "SourceFile and Characters are relative to the folder of the recipe.");
    }
    if (options->resource_name != NULL) {
        char     *name = utf8_of(options->resource_name);
        const int fits = name != NULL && strlen(name) < sizeof recipe->name;
        if (fits) {
            memcpy(recipe->name, name, strlen(name) + 1u);
        }
        free(name);
        if (!fits) {
            return refuse(error, EDDS_INVALID_INPUT, "invalid-resource-name", "The resource name does not fit a recipe.");
        }
    }
    paths->meta   = replace_tail(options->meta, 0, "");
    paths->fnt    = replace_tail(options->meta, 5u, "");
    paths->atlas  = replace_tail(options->meta, 9u, ".edds");
    paths->folder = folder_of(options->meta);
    if (paths->meta == NULL || paths->fnt == NULL || paths->atlas == NULL || paths->folder == NULL) {
        return refuse(error, EDDS_INTERNAL_FAILURE, "allocation-failed", "The font paths could not be prepared.");
    }
    paths->source = joined(paths->folder, recipe->source_file);
    if (recipe->characters[0] != '\0') {
        paths->characters = joined(paths->folder, recipe->characters);
    }
    if (paths->source == NULL || (recipe->characters[0] != '\0' && paths->characters == NULL)) {
        return refuse(error, EDDS_INVALID_INPUT, "recipe-path-invalid", "A recipe path is not valid UTF-8 for this platform.");
    }
    return EDDS_OK;
}

static edds_status recipe_from_input(const font_options *options, font_paths *paths, font_recipe *recipe,
    edds_error *error) {
    char *text;
    memset(recipe, 0, sizeof *recipe);
    recipe->font_size = options->size_seen ? options->size : FONT_DEFAULT_SIZE;
    paths->fnt        = replace_tail(options->output, 0, "");
    paths->meta       = replace_tail(options->output, 0, ".meta");
    paths->atlas      = replace_tail(options->output, 4u, ".edds");
    paths->folder     = folder_of(options->output);
    paths->source     = replace_tail(options->input, 0, "");
    if (options->characters != NULL) {
        paths->characters = replace_tail(options->characters, 0, "");
    }
    if (paths->fnt == NULL || paths->meta == NULL || paths->atlas == NULL || paths->folder == NULL ||
        paths->source == NULL || (options->characters != NULL && paths->characters == NULL)) {
        return refuse(error, EDDS_INTERNAL_FAILURE, "allocation-failed", "The font paths could not be prepared.");
    }
    text = utf8_of(options->resource_name);
    if (text == NULL || strlen(text) >= sizeof recipe->name) {
        free(text);
        return refuse(error, EDDS_INVALID_INPUT, "invalid-resource-name", "The resource name does not fit a recipe.");
    }
    memcpy(recipe->name, text, strlen(text) + 1u);
    free(text);
    text = relative_to(paths->folder, options->input);
    if (text == NULL || strlen(text) >= sizeof recipe->source_file) {
        free(text);
        return refuse(error, EDDS_INVALID_INPUT, "source-not-relative",
            "The source font must be reachable from the font's folder by a relative path.");
    }
    memcpy(recipe->source_file, text, strlen(text) + 1u);
    free(text);
    if (options->characters != NULL) {
        text = relative_to(paths->folder, options->characters);
        if (text == NULL || strlen(text) >= sizeof recipe->characters) {
            free(text);
            return refuse(error, EDDS_INVALID_INPUT, "characters-not-relative",
                "The character file must be reachable from the font's folder by a relative path.");
        }
        memcpy(recipe->characters, text, strlen(text) + 1u);
        free(text);
    }
    /* A font already here keeps its identity; a new one gets a new identity. */
    if (path_exists(paths->meta)) {
        FILE       *existing = open_input(paths->meta);
        edds_status status;
        if (existing == NULL) {
            return refuse(error, EDDS_INVALID_INPUT, "recipe-open-failed", "The existing font recipe could not be opened.");
        }
        status = font_recipe_guid(existing, recipe->guid, error);
        (void)fclose(existing);
        if (status != EDDS_OK) {
            return status;
        }
    }
    if (options->guid != NULL) {
        char     *guid  = utf8_of(options->guid);
        const int valid = guid != NULL && meta_valid_guid(guid);
        if (valid && recipe->guid[0] != '\0' && strcmp(recipe->guid, guid) != 0) {
            free(guid);
            return refuse(error, EDDS_INVALID_INPUT, "metadata-guid-mismatch",
                "Replacing a registered font must preserve its existing GUID character-for-character.");
        }
        if (valid) {
            memcpy(recipe->guid, guid, 17u);
        }
        free(guid);
        if (!valid) {
            return refuse(error, EDDS_INVALID_INVOCATION, "malformed-guid", "--guid takes 16 hexadecimal digits.");
        }
    }
    if (recipe->guid[0] == '\0') {
        return new_guid(paths->folder, recipe->guid, error);
    }
    return EDDS_OK;
}

static edds_status write_atlas(FILE *output, const font_output *font, edds_error *error) {
    edds_profile profile;
    edds_default_profile(&profile);
    /* Lossless, no mips: the shader reads level 0 and nothing else, and a lossy block would move edges. */
    profile.conversion         = EDDS_CONVERSION_NONE;
    profile.format_compress    = EDDS_COMPRESS_BEST;
    profile.compress_threshold = 100;
    profile.generate_mips      = 0;
    return edds_encode_rgba(font->atlas, font->atlas_width, font->atlas_height, 1, output, &profile,
        was_cancelled, NULL, error);
}

/** Builds the three files in sibling temporaries, then swaps them in together or not at all. */
static edds_status publish(const font_paths *paths, const font_recipe *recipe, const font_output *font,
    edds_error *error) {
    cli_artifact artifacts[3] = {
        { paths->atlas, NULL, NULL, 0, 0 }, { paths->fnt, NULL, NULL, 0, 0 }, { paths->meta, NULL, NULL, 0, 0 }
    };
    static const char *const write_stages[3]  = { "font-atlas-write", "font-fnt-write", "font-meta-write" };
    static const char *const backup_stages[3] = { "font-atlas-backup", "font-fnt-backup", "font-meta-backup" };
    static const char *const commit_stages[3] = { "font-atlas-commit", "font-fnt-commit", "font-meta-commit" };
    edds_status              status           = EDDS_OK;
    size_t                   backed = 0, committed = 0;
    for (size_t at = 0; at < 3u && status == EDDS_OK; ++at) {
        FILE *file = create_temporary(&artifacts[at], "new");
        if (file == NULL) {
            status = refuse(error, EDDS_INTERNAL_FAILURE, "temporary-open-failed",
                "A sibling temporary file could not be created.");
            break;
        }
        if (injected(write_stages[at])) {
            status = refuse(error, EDDS_INTERNAL_FAILURE, "injected-write", "A test fault was injected while writing.");
        } else if (at == 0) {
            status = write_atlas(file, font, error);
        } else if (at == 1) {
            if (fwrite(font->fnt, 1, font->fnt_size, file) != font->fnt_size) {
                status = refuse(error, EDDS_INTERNAL_FAILURE, "fnt-write-failed", "The FNT file could not be written.");
            }
        } else {
            status = font_recipe_write(file, recipe, error);
        }
        if (status == EDDS_OK && !sync_output(file, "font-flush")) {
            status = refuse(error, EDDS_INTERNAL_FAILURE, "temporary-flush-failed", "A temporary file could not be flushed.");
        }
        if (fclose(file) != 0 && status == EDDS_OK) {
            status = refuse(error, EDDS_INTERNAL_FAILURE, "temporary-close-failed", "A temporary file could not be closed.");
        }
    }
    if (status == EDDS_OK && was_cancelled(NULL)) {
        status = refuse(error, EDDS_CANCELLED, "cancelled", "The font generation was cancelled before it was published.");
    }
    for (; status == EDDS_OK && backed < 3u; ++backed) {
        if (!backup_artifact(&artifacts[backed], "old", backup_stages[backed])) {
            status = refuse(error, EDDS_INTERNAL_FAILURE, "artifact-backup-failed",
                "The previous font files could not be moved aside.");
            break;
        }
    }
    for (; status == EDDS_OK && committed < 3u; ++committed) {
        if (!commit_artifact(&artifacts[committed], commit_stages[committed])) {
            status = refuse(error, EDDS_INTERNAL_FAILURE, "artifact-commit-failed",
                "The new font files could not replace the previous ones; the previous ones were restored.");
            break;
        }
    }
    if (status != EDDS_OK) {
        /* A partial swap goes back: newest first, every file that moved returns to where it was. */
        int restored = 1;
        for (size_t at = 3u; at > 0; --at) {
            if (!rollback_artifact(&artifacts[at - 1u])) {
                restored = 0;
            }
        }
        if (!restored) {
            /* The backups stay where they are: they are the only copy of what was there. */
            status = refuse(error, EDDS_INTERNAL_FAILURE, "artifact-rollback-failed",
                "The font files could not all be put back; the previous ones are kept beside them as .enfusion-old temporaries.");
        }
    }
    for (size_t at = 0; at < 3u; ++at) {
        cleanup_artifact(&artifacts[at], status == EDDS_OK);
    }
    return status;
}

static void write_codes(const char *name, const uint32_t *codes, size_t count) {
    (void)printf(",\"%s\":[", name);
    for (size_t at = 0; at < count; ++at) {
        (void)printf("%s%u", at == 0 ? "" : ",", codes[at]);
    }
    putchar(']');
}

static int generate_command(const font_options *options) {
    font_paths      paths;
    font_recipe     recipe;
    font_characters characters = { NULL, 0 };
    font_output     font;
    font_request    request;
    edds_error      error;
    edds_status     status;
    uint8_t        *source = NULL, *text = NULL;
    size_t          source_size = 0, text_size = 0;
    char           *name = NULL;
    int             read;
    memset(&paths, 0, sizeof paths);
    memset(&font, 0, sizeof font);
    if (options->meta != NULL) {
        if (!ends_with(options->meta, ".fnt.meta")) {
            return invalid("invalid-options", "--meta names a .fnt.meta recipe.");
        }
        status = recipe_from_meta(options, &paths, &recipe, &error);
    } else {
        if (!ends_with(options->output, ".fnt")) {
            return invalid("invalid-options", "--output names a .fnt file.");
        }
        status = recipe_from_input(options, &paths, &recipe, &error);
    }
    if (status != EDDS_OK) {
        goto done;
    }
    read = read_file(paths.source, FONT_MAX_FILE_BYTES, &source, &source_size);
    if (read <= 0) {
        status = refuse(&error, EDDS_INVALID_INPUT, read < 0 ? "font-file-limit" : "input-open-failed",
            read < 0 ? "The source font is larger than a font may be." : "The source font could not be read.");
        goto done;
    }
    if (paths.characters != NULL) {
        read = read_file(paths.characters, FONT_MAX_CHARACTER_FILE_BYTES, &text, &text_size);
        if (read <= 0) {
            status = refuse(&error, EDDS_INVALID_INPUT, read < 0 ? "character-file-limit" : "characters-open-failed",
                read < 0 ? "The character file is larger than a character set may be." : "The character file could not be read.");
            goto done;
        }
        status = font_characters_parse(text, text_size, &characters, &error);
        if (status != EDDS_OK) {
            goto done;
        }
    }
    name = stem_of(paths.fnt);
    if (name == NULL) {
        status = refuse(&error, EDDS_INVALID_INPUT, "invalid-font-name", "The font file name is not valid UTF-8.");
        goto done;
    }
    request.data       = source;
    request.size       = source_size;
    request.characters = paths.characters != NULL ? &characters : NULL;
    request.font_size  = recipe.font_size;
    request.name       = name;
    status             = font_generate(&request, &font, was_cancelled, NULL, NULL, NULL, &error);
    if (status != EDDS_OK) {
        goto done;
    }
    status = publish(&paths, &recipe, &font, &error);
    if (status != EDDS_OK) {
        goto done;
    }
    (void)printf("{\"protocolVersion\":1,\"kind\":\"font-generate\",\"guid\":");
    json_string(recipe.guid);
    (void)printf(",\"glyphCount\":%u,\"rangeCount\":%u,\"pairCount\":%u,\"cell\":%u,"
                 "\"atlasWidth\":%u,\"atlasHeight\":%u",
        font.glyph_count, font.range_count, font.pair_count, font.cell, font.atlas_width, font.atlas_height);
    write_codes("missing", font.missing, font.missing_count);
    write_codes("drawn", font.drawn, font.drawn_count);
    fputs(",\"source\":{\"family\":", stdout);
    json_string(font.source.family);
    fputs(",\"style\":", stdout);
    json_string(font.source.style);
    fputs("}}\n", stdout);

done:
    free(source);
    free(text);
    free(name);
    font_characters_free(&characters);
    font_output_free(&font);
    free_paths(&paths);
    if (status != EDDS_OK) {
        return report_failure(status, &error);
    }
    return ferror(stdout) ? EDDS_INTERNAL_FAILURE : 0;
}

/* --- Inspect ---------------------------------------------------------------------------------- */

static int inspect_fnt(const cli_char *path) {
    FILE       *input = open_input(path);
    font_info   info;
    edds_error  error;
    edds_status status;
    if (input == NULL) {
        return report_failure(refuse(&error, EDDS_INVALID_INPUT, "input-open-failed", "The FNT file could not be opened."), &error);
    }
    status = font_inspect(input, &info, &error);
    (void)fclose(input);
    if (status != EDDS_OK) {
        return report_failure(status, &error);
    }
    fputs("{\"protocolVersion\":1,\"kind\":\"font-inspect\",\"name\":", stdout);
    json_string(info.name);
    (void)printf(",\"size\":%d,\"type\":%u,\"cell\":%d,\"capHeight\":%g,\"lineHeight\":%g,\"c\":%g,\"r\":%d,"
                 "\"bold\":%s,\"italic\":%s,\"glyphCount\":%u,\"pairCount\":%u,\"ranges\":[",
        (int)info.size, (unsigned)info.type, (int)info.cell, (double)info.cap_height, (double)info.line_height,
        (double)info.c, (int)info.r, info.bold ? "true" : "false", info.italic ? "true" : "false",
        info.glyph_count, info.pair_count);
    for (uint32_t at = 0; at < info.range_count; ++at) {
        (void)printf("%s{\"first\":%u,\"count\":%u}", at == 0 ? "" : ",", info.ranges[at].first, info.ranges[at].count);
    }
    fputs("]}\n", stdout);
    font_info_free(&info);
    return ferror(stdout) ? EDDS_INTERNAL_FAILURE : 0;
}

static int inspect_source(const cli_char *path) {
    uint8_t         *data = NULL;
    size_t           size = 0;
    font_source_info info;
    edds_error       error;
    edds_status      status;
    const int        read = read_file(path, FONT_MAX_FILE_BYTES, &data, &size);
    if (read <= 0) {
        return report_failure(refuse(&error, EDDS_INVALID_INPUT, read < 0 ? "font-file-limit" : "input-open-failed",
                                  "The source font could not be read within the font size limit."),
            &error);
    }
    status = font_source_describe(data, size, &info, &error);
    free(data);
    if (status != EDDS_OK) {
        return report_failure(status, &error);
    }
    fputs("{\"protocolVersion\":1,\"kind\":\"font-source\",\"family\":", stdout);
    json_string(info.family);
    fputs(",\"style\":", stdout);
    json_string(info.style);
    (void)printf(",\"unitsPerEm\":%u,\"glyphCount\":%u}\n", info.units_per_em, info.glyph_count);
    return ferror(stdout) ? EDDS_INTERNAL_FAILURE : 0;
}

int font_command(int argc, cli_char **argv) {
    font_options options;
    int          generate;
    cli_catch_interrupts();
    if (argc < 2 || (!equals(argv[1], "generate") && !equals(argv[1], "inspect"))) {
        return invalid("invalid-command", "Expected generate or inspect.");
    }
    generate = equals(argv[1], "generate");
    if (!parse_options(argc, argv, &options)) {
        return invalid("invalid-options", "The command options are incomplete, duplicated, or unsupported.");
    }
    if (!generate) {
        if (options.input == NULL || options.meta != NULL || options.output != NULL || options.resource_name != NULL ||
            options.size_seen || options.characters != NULL || options.guid != NULL || options.cancel_file != NULL) {
            return invalid("invalid-options", "inspect takes --input and nothing else.");
        }
        return ends_with(options.input, ".fnt") ? inspect_fnt(options.input) : inspect_source(options.input);
    }
    if (options.meta != NULL
            ? (options.input != NULL || options.output != NULL || options.size_seen || options.characters != NULL ||
                  options.guid != NULL)
            : (options.input == NULL || options.output == NULL || options.resource_name == NULL)) {
        return invalid("invalid-options",
            "generate takes --meta, or --input, --output and --resource-name with optional recipe flags.");
    }
    cli_watch_cancel_file(options.cancel_file);
    return generate_command(&options);
}
