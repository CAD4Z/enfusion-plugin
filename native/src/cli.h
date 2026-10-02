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

/**
 * The one machine error envelope on stdout, plus a line for a human on stderr. Returns the exit
 * code, which is the status's stable category.
 */
int report_failure(edds_status status, const edds_error *error);

/** `enfusion edds ...`; `argv[0]` is the area itself. */
int edds_command(int argc, cli_char **argv);

#endif
