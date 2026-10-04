/*
 * What every command of the executable shares with the process it runs in: being asked to stop
 * (SIGINT, or a cancel file a caller names), the failure injection the black-box tests use, its
 * arguments and the machine error envelope on stdout.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "cli.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t interrupted = 0;

static const cli_char *cancel_file = NULL;

static void on_interrupt(int signal_value) {
    (void)signal_value;
    interrupted = 1;
}

void cli_catch_interrupts(void) {
    (void)signal(SIGINT, on_interrupt);
}

void cli_watch_cancel_file(const cli_char *path) {
    cancel_file = path;
}

int was_cancelled(void *context) {
    (void)context;
    return interrupted != 0 || (cancel_file != NULL && path_exists(cancel_file));
}

int equals(const cli_char *left, const char *right) {
#ifdef _WIN32
    while (*left != L'\0' && *right != '\0' && *left == (wchar_t)(unsigned char)*right) {
        ++left;
        ++right;
    }
    return *left == L'\0' && *right == '\0';
#else
    return strcmp(left, right) == 0;
#endif
}

FILE *open_input(const cli_char *path) {
#ifdef _WIN32
    FILE *file = NULL;
    return _wfopen_s(&file, path, L"rb") == 0 ? file : NULL;
#else
    return fopen(path, "rb");
#endif
}

int injected(const char *stage) {
    const char *requested = getenv("EDDS_CONVERT_FAIL");
    return requested != NULL && strcmp(requested, stage) == 0;
}

edds_status injected_failure(edds_error *error, const char *stage) {
    memset(error, 0, sizeof *error);
    (void)snprintf(error->code, sizeof error->code, "injected-%s", stage);
    (void)snprintf(error->message, sizeof error->message,
        "A test fault was injected at transaction stage %s.", stage);
    return EDDS_INTERNAL_FAILURE;
}

int sync_output(FILE *file, const char *stage) {
    if (injected(stage)) {
        return 0;
    }
    if (fflush(file) != 0) {
        return 0;
    }
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

int unsigned_argument(const cli_char *text, uint32_t *value) {
    uint64_t parsed = 0;
    if (*text == 0) {
        return 0;
    }
    while (*text != 0) {
        unsigned digit;
        if (*text < (cli_char)'0' || *text > (cli_char)'9') {
            return 0;
        }
        digit = (unsigned)(*text - (cli_char)'0');
        parsed = parsed * 10u + digit;
        if (parsed > UINT32_MAX) {
            return 0;
        }
        ++text;
    }
    *value = (uint32_t)parsed;
    return 1;
}

void json_string(const char *text) {
    const unsigned char *at = (const unsigned char *)text;
    putchar('"');
    while (*at != 0) {
        switch (*at) {
            case '"': fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\b': fputs("\\b", stdout); break;
            case '\f': fputs("\\f", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:
                if (*at < 0x20u) {
                    (void)printf("\\u%04x", (unsigned)*at);
                } else {
                    putchar(*at);
                }
                break;
        }
        ++at;
    }
    putchar('"');
}

int report_failure(edds_status status, const edds_error *error) {
    const char *category = edds_status_category(status);
    const char *code = error != NULL && error->code[0] != '\0' ? error->code : "unspecified";
    const char *message = error != NULL && error->message[0] != '\0'
        ? error->message
        : "The EDDS operation failed.";
    (void)fprintf(stderr, "enfusion: %s: %s\n", category, message);
    fputs("{\"protocolVersion\":1,\"kind\":\"error\",\"error\":{\"category\":", stdout);
    json_string(category);
    fputs(",\"code\":", stdout);
    json_string(code);
    fputs(",\"message\":", stdout);
    json_string(message);
    fputs("}}\n", stdout);
    return (int)status;
}

char *utf8_of(const cli_char *value) {
#ifdef _WIN32
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
        NULL, 0, NULL, NULL);
    char *result;
    if (needed <= 0) {
        return NULL;
    }
    result = malloc((size_t)needed);
    if (result == NULL) {
        return NULL;
    }
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
            result, needed, NULL, NULL) != needed) {
        free(result);
        return NULL;
    }
    return result;
#else
    const size_t size = strlen(value);
    char *result = malloc(size + 1u);
    if (result != NULL) {
        memcpy(result, value, size + 1u);
    }
    return result;
#endif
}

cli_char *cli_of_utf8(const char *value) {
#ifdef _WIN32
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0);
    cli_char *result;
    if (needed <= 0) {
        return NULL;
    }
    result = malloc((size_t)needed * sizeof *result);
    if (result == NULL) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, result, needed) != needed) {
        free(result);
        return NULL;
    }
    return result;
#else
    const size_t size = strlen(value);
    cli_char *result = malloc(size + 1u);
    if (result != NULL) {
        memcpy(result, value, size + 1u);
    }
    return result;
#endif
}

void cli_request_cancel(void) {
    interrupted = 1;
}

void cli_cancel_through_file(void) {
    FILE *marker = NULL;
    if (cancel_file == NULL) {
        return;
    }
#ifdef _WIN32
    if (_wfopen_s(&marker, cancel_file, L"wb") != 0) {
        marker = NULL;
    }
#else
    marker = fopen(cancel_file, "wb");
#endif
    if (marker != NULL) {
        (void)fclose(marker);
    }
}
