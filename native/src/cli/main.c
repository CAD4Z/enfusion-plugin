#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "cli.h"

#include <string.h>

/** The version of this executable, as `enfusion protocol --machine` reports it. */
#define ENFUSION_TOOL_VERSION "0.2.0"

/**
 * One executable for every native area of the extension. The first argument names the area; what
 * follows is that area's own versioned CLI, unchanged by the others living next to it.
 * Returns the exit code: 0 on success, otherwise the status of the failure.
 */
int CLI_ENTRY(int argc, cli_char **argv) {
    edds_error error;

    /* `protocol --machine`: one JSON line with this version and the commands of every area. */
    if (argc == 3 && equals(argv[1], "protocol") && equals(argv[2], "--machine")) {
        puts("{\"protocolVersion\":1,\"kind\":\"protocol\",\"toolVersion\":\"" ENFUSION_TOOL_VERSION
             "\","
             "\"areas\":{\"edds\":[\"inspect\",\"preview\",\"convert\",\"batch\"],"
             "\"font\":[\"generate\",\"inspect\"]}}");
        return ferror(stdout) ? EDDS_INTERNAL_FAILURE : 0;
    }

    /* `edds ...` or `font ...`: that area's command takes the arguments from its own name on. */
    if (argc >= 2 && equals(argv[1], "edds")) {
        return edds_command(argc - 1, argv + 1);
    }

    if (argc >= 2 && equals(argv[1], "font")) {
        return font_command(argc - 1, argv + 1);
    }

    /* Anything else: the usage on stderr, and an invalid-command error envelope on stdout. */
    fputs(
        "usage:\n"
        "  enfusion protocol --machine\n"
        "  enfusion edds inspect|preview|convert|batch --machine --protocol 1 ...\n"
        "  enfusion font generate|inspect --machine --protocol 1 ...\n",
        stderr);

    memset(&error, 0, sizeof error);
    (void)snprintf(error.code, sizeof error.code, "invalid-command");
    (void)snprintf(error.message, sizeof error.message,
        "Expected protocol or an area: edds, font.");

    return report_failure(EDDS_INVALID_INVOCATION, &error);
}
