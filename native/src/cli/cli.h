/*
 * What every area of the `enfusion` executable shares: the platform's own argument characters,
 * the process it runs in, the machine error envelope and the exit categories it carries, and the
 * transaction that publishes files.
 */
#ifndef ENFUSION_CLI_H
#define ENFUSION_CLI_H

#include <edds/edds.h>

#include <stdio.h>

/*
 * The platform's own argument characters. On Windows the program starts at `wmain`, and an
 * argument or a path is a string of `wchar_t`; elsewhere it starts at `main`, with `char`. The
 * `cli_` names stand for the calls on such strings, and for the process id, on each platform.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <process.h>
#include <wchar.h>

typedef wchar_t cli_char;

#define CLI_ENTRY  wmain
#define cli_remove _wremove
#define cli_rename _wrename
#define cli_strlen wcslen
#define cli_strcmp wcscmp
#define cli_getpid _getpid
#else
#include <sys/stat.h>
#include <unistd.h>

typedef char cli_char;

#define CLI_ENTRY  main
#define cli_remove remove
#define cli_rename rename
#define cli_strlen strlen
#define cli_strcmp strcmp
#define cli_getpid getpid
#endif

/* --- The process: process.c ------------------------------------------------------------------- */

/** Whether a platform argument is exactly this ASCII text. */
int equals(const cli_char *left, const char *right);

/** Reads an argument of decimal digits that fits 32 bits into `value`: 1, or 0 when it is not. */
int unsigned_argument(const cli_char *text, uint32_t *value);

/** A platform string as UTF-8, and UTF-8 as a platform string; malloc'd, or NULL. */
char *utf8_of(const cli_char *value);
cli_char *cli_of_utf8(const char *value);

/** Opens a file to read its bytes; NULL when it cannot be opened. */
FILE *open_input(const cli_char *path);

/**
 * The one machine error envelope on stdout, plus a line for a human on stderr. Returns the exit
 * code, which is the status's stable category.
 */
int report_failure(edds_status status, const edds_error *error);

/** Writes `text` to stdout as a JSON string, quoted and escaped. */
void json_string(const char *text);

/**
 * SIGINT, or the cancel file a caller named, asks a command to stop at its next safe point.
 * `was_cancelled` is nonzero once either has happened.
 */
void cli_catch_interrupts(void);
void cli_watch_cancel_file(const cli_char *path);
int was_cancelled(void *context);

/** Asks the running command to stop at its next safe point, as SIGINT does. */
void cli_request_cancel(void);

/** Creates the cancel file this process watches, as a caller asking it to stop would. */
void cli_cancel_through_file(void);

/**
 * `EDDS_CONVERT_FAIL` names the one transaction stage a black-box test makes fail. Nonzero when
 * that is `stage`.
 */
int injected(const char *stage);

/** The error of a failure injected at that transaction stage; the status is an internal failure. */
edds_status injected_failure(edds_error *error, const char *stage);

/** Flushes a written file all the way to the disk: 1, or 0 when that failed or a test failed it. */
int sync_output(FILE *file, const char *stage);

/* --- Publishing: publish.c -------------------------------------------------------------------- */

/** ASCII case folding, the only folding a path suffix or a Windows path comparison needs. */
int ascii_lower(cli_char value);

/** Whether `path` ends with `suffix`, in any ASCII case; `suffix` is written in lowercase. */
int ends_with(const cli_char *path, const char *suffix);

/** Whether a character separates the folders of a path: `/` or `\`. */
int is_separator(cli_char value);

/** The normal form of a path as the system resolves it; malloc'd, or NULL. */
cli_char *canonical_path(const cli_char *path);

/** Whether two paths name one file, however each of them is spelled. */
int same_path(const cli_char *left, const cli_char *right);

/** The path with an ASCII suffix appended; malloc'd, or NULL. */
cli_char *append_suffix(const cli_char *path, const char *suffix);

/** Whether there is a file at `path` that can be opened for reading. */
int path_exists(const cli_char *path);

/** A file as a caller saw it: whether it was there, its size, and its time in milliseconds. */
typedef struct file_revision {
    int      exists;
    uint64_t size;
    uint64_t modified;
} file_revision;

/**
 * The file at `path` as it is now. A missing path, or one that is not a plain file, is a revision
 * that does not exist; 0 only when the system could not tell.
 */
int revision_of(const cli_char *path, file_revision *revision);

/** `size:mtime`, or `missing`. 0 when the text is neither. */
int parse_revision(const cli_char *text, file_revision *revision);

/** Whether two revisions are one write of one file. */
int same_revision(const file_revision *left, const file_revision *right);

/**
 * One destination of an atomic publish: built in a sibling temporary
 * `<target>.enfusion-<kind>-<pid>-<attempt>.tmp`, the old file moved aside to another. The
 * extension recovers what a dead process left under that name.
 */
typedef struct cli_artifact {
    /** The destination, the temporary the new file is built in, and where the old one went. */
    const cli_char *target;
    cli_char       *temporary;
    cli_char       *backup;

    /** Whether a file was at the destination before, and whether the new one is there now. */
    int had_previous;
    int committed;
} cli_artifact;

/** Creates and opens the sibling temporary the new file is written to; NULL when it could not. */
FILE *create_temporary(cli_artifact *artifact, const char *kind);

/** Moves any file at the destination aside, to a sibling of its own: 1, or 0 when it could not. */
int backup_artifact(cli_artifact *artifact, const char *kind, const char *stage);

/** Moves the temporary onto the destination: 1, or 0 when it could not. */
int commit_artifact(cli_artifact *artifact, const char *stage);

/** Puts the destination back the way the publish found it; 0 when that could not be done. */
int rollback_artifact(cli_artifact *artifact);

/** Removes the temporary, and after a success the backup too, and frees both names. */
void cleanup_artifact(cli_artifact *artifact, int success);

/**
 * The host can recover after a killed process, including between the two destination renames.
 * Pending is flushed before either original moves; its rename to committed is the pair's commit
 * point. Backups are discarded only after that point, and the marker is discarded last.
 */
typedef struct cli_transaction {
    /** The marker's two names: pending, and committed, which pending is renamed to. */
    cli_char *pending;
    cli_char *committed;

    /** Whether the pending marker was created. */
    int started;
} cli_transaction;

/** Creates the pending marker beside `output` and flushes it to the disk: 1, or 0 on failure. */
int begin_transaction(cli_transaction *transaction, const cli_char *output);

/** Frees the marker's names; once the publish is resolved, removes the marker files as well. */
void cleanup_transaction(cli_transaction *transaction, int resolved);

/** Ends the process at that publish stage when a black-box test asks for a crash there. */
void crash_if_requested(const char *stage);

/* --- The areas: edds.c and font.c ------------------------------------------------------------- */

/** `enfusion edds ...`; `argv[0]` is the area itself. Returns the exit code. */
int edds_command(int argc, cli_char **argv);

/** `enfusion font ...`; `argv[0]` is the area itself. Returns the exit code. */
int font_command(int argc, cli_char **argv);

#endif
