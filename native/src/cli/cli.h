/*
 * What every area of the `enfusion` executable shares: the platform's own argument characters,
 * the process it runs in, the machine error envelope and the exit categories it carries, and the
 * transaction that publishes files.
 */
#ifndef ENFUSION_CLI_H
#define ENFUSION_CLI_H

#include <edds/edds.h>

#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <process.h>
#include <wchar.h>
typedef wchar_t cli_char;
#define CLI_ENTRY wmain
#define cli_remove _wremove
#define cli_rename _wrename
#define cli_strlen wcslen
#define cli_strcmp wcscmp
#define cli_getpid _getpid
#else
#include <sys/stat.h>
#include <unistd.h>
typedef char cli_char;
#define CLI_ENTRY main
#define cli_remove remove
#define cli_rename rename
#define cli_strlen strlen
#define cli_strcmp strcmp
#define cli_getpid getpid
#endif

/* --- The process: process.c ------------------------------------------------------------------- */

/** Whether a platform argument is exactly this ASCII text. */
int equals(const cli_char *left, const char *right);
int unsigned_argument(const cli_char *text, uint32_t *value);
char *utf8_of(const cli_char *value);
cli_char *cli_of_utf8(const char *value);
FILE *open_input(const cli_char *path);

/**
 * The one machine error envelope on stdout, plus a line for a human on stderr. Returns the exit
 * code, which is the status's stable category.
 */
int report_failure(edds_status status, const edds_error *error);
void json_string(const char *text);

/** SIGINT, or the cancel file a caller named, asks a command to stop at its next safe point. */
void cli_catch_interrupts(void);
void cli_watch_cancel_file(const cli_char *path);
int was_cancelled(void *context);
/** Asks the running command to stop at its next safe point, as SIGINT does. */
void cli_request_cancel(void);
/** Creates the cancel file this process watches, as a caller asking it to stop would. */
void cli_cancel_through_file(void);

/** `EDDS_CONVERT_FAIL` names the one transaction stage a black-box test makes fail. */
int injected(const char *stage);
/** The error of a failure injected at that transaction stage. */
edds_status injected_failure(edds_error *error, const char *stage);
int sync_output(FILE *file, const char *stage);

/* --- Publishing: publish.c -------------------------------------------------------------------- */

/** ASCII case folding, the only folding a path suffix or a Windows path comparison needs. */
int ascii_lower(cli_char value);
int ends_with(const cli_char *path, const char *suffix);
int is_separator(cli_char value);
/** The normal form of a path as the system resolves it; malloc'd, or NULL. */
cli_char *canonical_path(const cli_char *path);
/** Whether two paths name one file, however each of them is spelled. */
int same_path(const cli_char *left, const cli_char *right);
/** The path with an ASCII suffix appended; malloc'd, or NULL. */
cli_char *append_suffix(const cli_char *path, const char *suffix);
int path_exists(const cli_char *path);

/** A file as a caller saw it: whether it was there, its size, and its time in milliseconds. */
typedef struct file_revision {
    int exists;
    uint64_t size;
    uint64_t modified;
} file_revision;

int revision_of(const cli_char *path, file_revision *revision);
/** `size:mtime`, or `missing`. */
int parse_revision(const cli_char *text, file_revision *revision);
int same_revision(const file_revision *left, const file_revision *right);

/**
 * One destination of an atomic publish: built in a sibling temporary
 * `<target>.enfusion-<kind>-<pid>-<attempt>.tmp`, the old file moved aside to another. The
 * extension recovers what a dead process left under that name.
 */
typedef struct cli_artifact {
    const cli_char *target;
    cli_char *temporary;
    cli_char *backup;
    int had_previous;
    int committed;
} cli_artifact;

FILE *create_temporary(cli_artifact *artifact, const char *kind);
int backup_artifact(cli_artifact *artifact, const char *kind, const char *stage);
int commit_artifact(cli_artifact *artifact, const char *stage);
/** Puts the destination back the way the publish found it; 0 when that could not be done. */
int rollback_artifact(cli_artifact *artifact);
void cleanup_artifact(cli_artifact *artifact, int success);

/*
 * The host can recover after a killed process, including between the two destination renames.
 * Pending is flushed before either original moves; its rename to committed is the pair's commit
 * point. Backups are discarded only after that point, and the marker is discarded last.
 */
typedef struct cli_transaction {
    cli_char *pending;
    cli_char *committed;
    int started;
} cli_transaction;

int begin_transaction(cli_transaction *transaction, const cli_char *output);
void cleanup_transaction(cli_transaction *transaction, int resolved);
/** Ends the process at that publish stage when a black-box test asks for a crash there. */
void crash_if_requested(const char *stage);

/* --- The areas: edds.c and font.c ------------------------------------------------------------- */

/** `enfusion edds ...`; `argv[0]` is the area itself. */
int edds_command(int argc, cli_char **argv);

/** `enfusion font ...`; `argv[0]` is the area itself. */
int font_command(int argc, cli_char **argv);

#endif
