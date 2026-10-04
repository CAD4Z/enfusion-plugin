/*
 * Publishing files the way a crash cannot half-do: paths compared as the system resolves them,
 * file revisions, sibling temporaries moved into place with rollback, and the journal the host
 * recovers a pair from after the process died.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "cli.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static FILE *open_output(const cli_char *path) {
#ifdef _WIN32
    FILE *file = NULL;
    return _wfopen_s(&file, path, L"w+b") == 0 ? file : NULL;
#else
    return fopen(path, "w+b");
#endif
}

int ascii_lower(cli_char value) {
    return value >= (cli_char)'A' && value <= (cli_char)'Z'
        ? value + ((cli_char)'a' - (cli_char)'A')
        : value;
}

int ends_with(const cli_char *path, const char *suffix) {
    const size_t path_size   = cli_strlen(path);
    const size_t suffix_size = strlen(suffix);
    if (path_size < suffix_size) {
        return 0;
    }
    for (size_t at = 0; at < suffix_size; ++at) {
        if (ascii_lower(path[path_size - suffix_size + at]) != (unsigned char)suffix[at]) {
            return 0;
        }
    }
    return 1;
}

int is_separator(cli_char value) {
    return value == (cli_char)'/' || value == (cli_char)'\\';
}

/**
 * A destination reduced to what a comparison should look at: case, either separator, a doubled or
 * trailing one, a `.` segment, and a `..` the path walks back through. A root is never walked
 * above, so a drive and a UNC share stay whole. Returns a malloc'd normal form, or NULL.
 */
static cli_char *normalized_path(const cli_char *path) {
    const size_t length = cli_strlen(path);
    cli_char    *result = malloc((length + 2u) * sizeof *result);
    /* Where each segment that may still be walked off begins; a root segment is never in here. */
    size_t      *marks  = malloc((length / 2u + 2u) * sizeof *marks);
    size_t       depth  = 0;
    size_t       out    = 0;
    size_t       at     = 0;
    size_t       prefix;
    size_t       roots;
    int          share;
    int          anchored;
    if (result == NULL || marks == NULL) {
        free(result);
        free(marks);
        return NULL;
    }
    /* A `/C:` the way a Uri.path spells a drive is not a rooted path, it is the drive itself. */
    if (length >= 3u && is_separator(path[0]) && path[2] == (cli_char)':' &&
        ascii_lower(path[1]) >= (cli_char)'a' && ascii_lower(path[1]) <= (cli_char)'z') {
        at = 1u;
    }
    share = length >= at + 2u && is_separator(path[at]) && is_separator(path[at + 1u]);
    if (share) {
        result[out++]  = (cli_char)'/';
        result[out++]  = (cli_char)'/';
        at            += 2u;
    } else if (at < length && is_separator(path[at])) {
        result[out++] = (cli_char)'/';
        ++at;
    }
    prefix   = out;
    /* The server and the share name a UNC root; a bare drive letter is a root of its own. */
    roots    = share ? 2u : 0u;
    anchored = prefix > 0u;

    while (at < length) {
        const size_t start = at;
        size_t       size;
        int          parent;
        while (at < length && !is_separator(path[at])) {
            ++at;
        }
        size = at - start;
        if (at < length) {
            ++at;
        }
        if (size == 0u || (size == 1u && path[start] == (cli_char)'.')) {
            continue;
        }
        parent = size == 2u && path[start] == (cli_char)'.' && path[start + 1u] == (cli_char)'.';
        if (parent && depth > 0u) {
            out = marks[--depth];
            continue;
        }
        /* Nothing sits above a root, so the walk a rooted path cannot take is dropped, not kept. */
        if (parent && anchored) {
            continue;
        }
        if (out > prefix) {
            result[out++] = (cli_char)'/';
        }
        /* A root segment and a `..` a relative path kept are both segments nothing walks off. */
        if (roots > 0u) {
            --roots;
        } else if (!parent) {
            marks[depth++] = out > prefix ? out - 1u : out;
        }
        for (size_t copy = 0; copy < size; ++copy) {
            result[out++] = (cli_char)ascii_lower(path[start + copy]);
        }
        if (!anchored && depth == 1u && out == 2u && result[1] == (cli_char)':') {
            /* That first segment was a drive letter after all: it becomes the root behind us. */
            depth    = 0;
            anchored = 1;
        }
    }
    result[out] = 0;
    free(marks);
    return result;
}

/** Case and separator only: what a comparison can still say when there is no memory to normalize. */
static int same_path_literally(const cli_char *left, const cli_char *right) {
    while (*left != 0 && *right != 0) {
        const cli_char a = is_separator(*left) ? (cli_char)'/' : (cli_char)ascii_lower(*left);
        const cli_char b = is_separator(*right) ? (cli_char)'/' : (cli_char)ascii_lower(*right);
        if (a != b) {
            return 0;
        }
        ++left;
        ++right;
    }
    return *left == 0 && *right == 0;
}

#ifdef _WIN32
/*
 * Windows has spellings of one file that no text comparison sees: a trailing dot or space, which
 * it trims from a name, an 8.3 short name, a junction in the path. The full path settles the first;
 * a file that is there is also compared by what it is — its volume and its index on that volume.
 */
static cli_char *full_path_of(const cli_char *path) {
    const DWORD needed = GetFullPathNameW(path, 0, NULL, NULL);
    cli_char   *result;
    DWORD       written;
    if (needed == 0) {
        return NULL;
    }
    result = malloc(needed * sizeof *result);
    if (result == NULL) {
        return NULL;
    }
    written = GetFullPathNameW(path, needed, result, NULL);
    if (written == 0 || written >= needed) {
        free(result);
        return NULL;
    }
    return result;
}

static int same_file(const cli_char *left, const cli_char *right) {
    BY_HANDLE_FILE_INFORMATION left_facts, right_facts;
    const DWORD                share        = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE                     left_handle  = CreateFileW(left, 0, share, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    HANDLE                     right_handle = CreateFileW(right, 0, share, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    int                        same         = 0;
    if (left_handle != INVALID_HANDLE_VALUE && right_handle != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(left_handle, &left_facts) &&
        GetFileInformationByHandle(right_handle, &right_facts)) {
        same = left_facts.dwVolumeSerialNumber == right_facts.dwVolumeSerialNumber &&
            left_facts.nFileIndexHigh == right_facts.nFileIndexHigh &&
            left_facts.nFileIndexLow == right_facts.nFileIndexLow;
    }
    if (left_handle != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(left_handle);
    }
    if (right_handle != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(right_handle);
    }
    return same;
}
#endif

/** The normal form of the path as the system resolves it; a malloc'd string, or NULL. */
cli_char *canonical_path(const cli_char *path) {
#ifdef _WIN32
    cli_char *full   = full_path_of(path);
    cli_char *result = normalized_path(full != NULL ? full : path);
    free(full);
    return result;
#else
    return normalized_path(path);
#endif
}

int same_path(const cli_char *left, const cli_char *right) {
    cli_char *canonical_left;
    cli_char *canonical_right;
    int       same;
#ifdef _WIN32
    if (same_file(left, right)) {
        return 1;
    }
#endif
    canonical_left  = canonical_path(left);
    canonical_right = canonical_path(right);
    /* Out of memory, the coarser answer is the safe one: a collision missed writes two jobs to
       one file, while a collision seen twice only refuses work the caller can ask for again. */
    same            = canonical_left == NULL || canonical_right == NULL
                   ? same_path_literally(left, right)
                   : cli_strcmp(canonical_left, canonical_right) == 0;
    free(canonical_left);
    free(canonical_right);
    return same;
}

static cli_char *temporary_path(const cli_char *output, const char *kind, unsigned attempt) {
    const size_t base = cli_strlen(output);
    cli_char    *path = malloc((base + 96u) * sizeof *path);
    if (path == NULL) {
        return NULL;
    }
#ifdef _WIN32
    if (swprintf(path, base + 96u, L"%ls.enfusion-%hs-%d-%u.tmp",
            output, kind, cli_getpid(), attempt) < 0) {
#else
    if (snprintf(path, base + 96u, "%s.enfusion-%s-%d-%u.tmp",
            output, kind, cli_getpid(), attempt) < 0) {
#endif
        free(path);
        return NULL;
    }
    return path;
}

int path_exists(const cli_char *path) {
    FILE *file = open_input(path);
    if (file == NULL) {
        return 0;
    }
    (void)fclose(file);
    return 1;
}

int revision_of(const cli_char *path, file_revision *revision) {
    memset(revision, 0, sizeof *revision);
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    ULARGE_INTEGER            size;
    ULARGE_INTEGER            time;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attributes)) {
        const DWORD code = GetLastError();
        return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
    }
    if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return 1;
    }
    size.HighPart      = attributes.nFileSizeHigh;
    size.LowPart       = attributes.nFileSizeLow;
    time.HighPart      = attributes.ftLastWriteTime.dwHighDateTime;
    time.LowPart       = attributes.ftLastWriteTime.dwLowDateTime;
    revision->exists   = 1;
    revision->size     = size.QuadPart;
    revision->modified = time.QuadPart / 10000u - 11644473600000u;
#else
    struct stat attributes;
    if (stat(path, &attributes) != 0) {
        return errno == ENOENT;
    }
    if (!S_ISREG(attributes.st_mode)) {
        return 1;
    }
    revision->exists   = 1;
    revision->size     = (uint64_t)attributes.st_size;
    revision->modified = (uint64_t)attributes.st_mtim.tv_sec * 1000u +
        (uint64_t)attributes.st_mtim.tv_nsec / 1000000u;
#endif
    return 1;
}

int parse_revision(const cli_char *text, file_revision *revision) {
    uint64_t values[2] = { 0, 0 };
    memset(revision, 0, sizeof *revision);
    if (equals(text, "missing")) {
        return 1;
    }
    for (unsigned part = 0; part < 2u; ++part) {
        int digits = 0;
        while (*text >= (cli_char)'0' && *text <= (cli_char)'9') {
            const unsigned digit = (unsigned)(*text - (cli_char)'0');
            if (values[part] > (UINT64_MAX - digit) / 10u) {
                return 0;
            }
            values[part] = values[part] * 10u + digit;
            ++text;
            digits = 1;
        }
        if (!digits || (part == 0u ? *text++ != (cli_char)':' : *text != 0)) {
            return 0;
        }
    }
    revision->exists   = 1;
    revision->size     = values[0];
    revision->modified = values[1];
    return 1;
}

/*
 * The caller's time is read through a millisecond `Date`, which rounds the file time, while this
 * side truncates it: one millisecond apart is the same write. The size still has to match exactly.
 */
int same_revision(const file_revision *left, const file_revision *right) {
    const uint64_t later   = left->modified > right->modified ? left->modified : right->modified;
    const uint64_t earlier = left->modified > right->modified ? right->modified : left->modified;
    return left->exists == right->exists && (!left->exists || (left->size == right->size && later - earlier <= 1u));
}

cli_char *append_suffix(const cli_char *path, const char *suffix) {
    const size_t path_size   = cli_strlen(path);
    const size_t suffix_size = strlen(suffix);
    cli_char    *result      = malloc((path_size + suffix_size + 1u) * sizeof *result);
    if (result == NULL) {
        return NULL;
    }
    memcpy(result, path, path_size * sizeof *result);
    for (size_t at = 0; at < suffix_size; ++at) {
        result[path_size + at] = (cli_char)(unsigned char)suffix[at];
    }
    result[path_size + suffix_size] = 0;
    return result;
}

FILE *create_temporary(cli_artifact *artifact, const char *kind) {
    for (unsigned attempt = 0; attempt < 32u; ++attempt) {
        FILE *file;
        free(artifact->temporary);
        artifact->temporary = temporary_path(artifact->target, kind, attempt);
        if (artifact->temporary == NULL || path_exists(artifact->temporary)) {
            continue;
        }
        file = open_output(artifact->temporary);
        if (file != NULL) {
            return file;
        }
    }
    return NULL;
}

int backup_artifact(cli_artifact *artifact, const char *kind, const char *stage) {
    artifact->had_previous = path_exists(artifact->target);
    if (!artifact->had_previous) {
        return 1;
    }
    if (injected(stage)) {
        return 0;
    }
    for (unsigned attempt = 0; attempt < 32u; ++attempt) {
        free(artifact->backup);
        artifact->backup = temporary_path(artifact->target, kind, attempt);
        if (artifact->backup != NULL && !path_exists(artifact->backup) &&
            cli_rename(artifact->target, artifact->backup) == 0) {
            return 1;
        }
    }
    return 0;
}

int commit_artifact(cli_artifact *artifact, const char *stage) {
    if (injected(stage)) {
        return 0;
    }
    if (cli_rename(artifact->temporary, artifact->target) != 0) {
        return 0;
    }
    artifact->committed = 1;
    return 1;
}

int rollback_artifact(cli_artifact *artifact) {
    if (artifact->committed) {
        if (path_exists(artifact->target) && cli_remove(artifact->target) != 0) {
            return 0;
        }
        artifact->committed = 0;
    }
    if (artifact->had_previous && artifact->backup != NULL && path_exists(artifact->backup)) {
        return cli_rename(artifact->backup, artifact->target) == 0;
    }
    return !artifact->had_previous || path_exists(artifact->target);
}

void cleanup_artifact(cli_artifact *artifact, int success) {
    if (artifact->temporary != NULL) {
        (void)cli_remove(artifact->temporary);
    }
    if (success && artifact->backup != NULL) {
        (void)cli_remove(artifact->backup);
    }
    free(artifact->temporary);
    free(artifact->backup);
    artifact->temporary = NULL;
    artifact->backup    = NULL;
}

int begin_transaction(cli_transaction *transaction, const cli_char *output) {
    for (unsigned attempt = 0; attempt < 32u; ++attempt) {
        FILE *file;
        int   flushed;
        free(transaction->pending);
        free(transaction->committed);
        transaction->pending   = temporary_path(output, "pending", attempt);
        transaction->committed = temporary_path(output, "committed", attempt);
        if (transaction->pending == NULL || transaction->committed == NULL) {
            return 0;
        }
        if (path_exists(transaction->pending) || path_exists(transaction->committed)) {
            continue;
        }
        file = open_output(transaction->pending);
        if (file == NULL) {
            return 0;
        }
        transaction->started = 1;
        flushed              = sync_output(file, "transaction-flush");
        if (fclose(file) != 0) {
            flushed = 0;
        }
        return flushed;
    }
    /* None of the occupied slots belongs to this transaction. */
    free(transaction->pending);
    free(transaction->committed);
    transaction->pending = transaction->committed = NULL;
    return 0;
}

void cleanup_transaction(cli_transaction *transaction, int resolved) {
    if (resolved && transaction->started) {
        if (transaction->pending != NULL) {
            (void)cli_remove(transaction->pending);
        }
        if (transaction->committed != NULL) {
            (void)cli_remove(transaction->committed);
        }
    }
    free(transaction->pending);
    free(transaction->committed);
}

void crash_if_requested(const char *stage) {
    if (injected(stage)) {
        _Exit(EDDS_INTERNAL_FAILURE);
    }
}
