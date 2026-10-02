/*
 * What every area of the `enfusion` executable shares: the platform's own argument characters,
 * the machine error envelope, and the exit categories it carries.
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

/** Whether a platform argument is exactly this ASCII text. */
int equals(const cli_char *left, const char *right);
int unsigned_argument(const cli_char *text, uint32_t *value);

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

FILE *open_input(const cli_char *path);
int path_exists(const cli_char *path);
char *utf8_of(const cli_char *value);
cli_char *cli_of_utf8(const char *value);

/** ASCII case folding, the only folding a path suffix or a Windows path comparison needs. */
int ascii_lower(cli_char value);
int ends_with(const cli_char *path, const char *suffix);
int is_separator(cli_char value);

/** `EDDS_CONVERT_FAIL` names the one transaction stage a black-box test makes fail. */
int injected(const char *stage);
int sync_output(FILE *file, const char *stage);

/**
 * One destination of an atomic publish: built in a sibling temporary
 * `<target>.enfusion-<kind>-<pid>-<attempt>.tmp`, the old file moved aside to another. The
 * extension removes the ones a dead process left by that name.
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
void rollback_artifact(cli_artifact *artifact);
void cleanup_artifact(cli_artifact *artifact, int success);

/** `enfusion edds ...`; `argv[0]` is the area itself. */
int edds_command(int argc, cli_char **argv);

/** `enfusion font ...`; `argv[0]` is the area itself. */
int font_command(int argc, cli_char **argv);

#endif
