////////////////////////////////////////////////////////////////////////////////
// Copyright 2026 Tom G. Huang <tomghuang@gmail.com>
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

////////////////////////////////////////////////////////////////////////////////
/// @file
/// @brief The `ohmg` entry point: global options, then dispatch to a subcommand.
///
/// @details
/// argtable3 owns the command tree; this file finds where the subcommand starts
/// in argv, parses the global options that come before it (-v/-q/--json/
/// --config), applies them to deps/log, then hands the rest of argv to the
/// matching cmd_*_proc(), which lives with its module under src/cli/.

#include "app_config.h"
#include "app_version.h"
#include "cli/cmd_bm.h"
#include "cli/cmd_dm.h"
#include "cli/cmd_han.h"
#include "cli/cmd_help.h"
#include "cli/cmd_info.h"
#include "cli/cmd_sm.h"
#include "utils.h"

#include <argtable3.h>
#include <log.h>
#include <uv.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

////////////////////////////////////////////////////////////////////////////////
/// Finds where the subcommand starts in argv, so the global options can be
/// parsed on their own (argtable3 wants a self-contained argv).
///
/// @param argc Argument count as received by main().
/// @param argv Argument vector as received by main().
/// @return The index of the first registered subcommand in argv, or -1 when
///         argv holds no subcommand at all.
static int arg_cmd_find_subcmd_index(int argc, char* argv[])
{
    arg_cmd_itr_t itr = arg_cmd_itr_create();
    int subcmd_index = argc;
    do {
        const char* subcmd = arg_cmd_itr_key(itr);

        // Search argv for this subcmd, skip argv[0] (program name)
        for (int i = 1; i < argc; ++i) {
            if (strcmp(argv[i], subcmd) == 0) {
                if (i < subcmd_index) {
                    subcmd_index = i;
                }
            }
        }
    } while (arg_cmd_itr_advance(itr));
    arg_cmd_itr_destroy(itr);

    return subcmd_index == argc ? -1 : subcmd_index;
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// The `ohmg` entry point: register the command tree, parse the global options
/// that precede the subcommand, configure logging, then dispatch.
///
/// @param argc Argument count.
/// @param argv Argument vector.
/// @return The subcommand's exit code, or EXIT_FAILURE / 1 on a usage error.
int main(int argc, char* argv[])
{
    app_info_t info = {0};
    int rv = get_app_info(&info);
    if (rv != 0) {
        log_error("failed to get app info");
        return EXIT_FAILURE;
    }

    arg_cmd_init();
    arg_set_module_name(APP_NAME);
    arg_set_module_version(APP_VER_MAJOR, APP_VER_MINOR, APP_VER_PATCH, APP_VER_TAG);
    arg_cmd_register(cmd_help_name(), cmd_help_proc, cmd_help_desc(), &info);
    arg_cmd_register(cmd_bm_name(), cmd_bm_proc, cmd_bm_desc(), &info);
    arg_cmd_register(cmd_sm_name(), cmd_sm_proc, cmd_sm_desc(), &info);
    arg_cmd_register(cmd_han_name(), cmd_han_proc, cmd_han_desc(), &info);
    arg_cmd_register(cmd_info_name(), cmd_info_proc, cmd_info_desc(), &info);
    arg_cmd_register(cmd_dm_name(), cmd_dm_proc, cmd_dm_desc(), &info);
    arg_dstr_t res = arg_dstr_create();
    if (argc == 1) {
        arg_make_get_help_msg(res);
        out_data("%s", arg_dstr_cstr(res));
        arg_dstr_destroy(res);
        arg_cmd_uninit();
        return EXIT_FAILURE;
    }

    // Parse global options and find the subcommand.

    arg_lit_t* verbose = NULL;
    arg_lit_t* quiet = NULL;
    arg_rem_t* quiet_rem = NULL;
    arg_lit_t* json = NULL;
    arg_str_t* config = NULL;
    arg_end_t* end = arg_end(20);

    verbose = arg_litn("v", "verbose", 0, 2, "enable verbose logging");
    quiet = arg_lit0("q", "quiet", "suppress human-oriented output");
    quiet_rem = arg_rem(NULL, "keep machine output and critical errors");
    json = arg_lit0(NULL, "json", "output in JSON format");
    config = arg_str0(NULL, "config", "<path>", "specify configuration file");

    void* argtable[] = {verbose, quiet, quiet_rem, json, config, end};
    if (arg_nullcheck(argtable) != 0) {
        log_error("failed to allocate argtable");
        arg_dstr_destroy(res);
        arg_cmd_uninit();
        return 1;
    }

    config->sval[0] = info.config_path;

    int subcmd_index = arg_cmd_find_subcmd_index(argc, argv);
    if (subcmd_index == -1) {
        // No subcommand found
        arg_make_get_help_msg(res);
        printf("%s", arg_dstr_cstr(res));
        arg_dstr_destroy(res);
        arg_cmd_uninit();
        return 1;
    }

    int nerrors = arg_parse(subcmd_index, argv, argtable);
    int exitcode = 0;
    if (arg_make_syntax_err_help_msg(res, "forge", 0, nerrors, argtable, end, &exitcode)) {
        printf("%s", arg_dstr_cstr(res));
        arg_dstr_destroy(res);
        arg_cmd_uninit();
        return exitcode;
    }

    // Start the application based on the command-line options and arguments

    int log_level = LOG_WARN;
    if (verbose->count > 0) {
        switch (verbose->count) {
        case 1:
            log_level = LOG_INFO;
            break;
        case 2:
            log_level = LOG_DEBUG;
            break;
        default:
            log_level = LOG_TRACE;
            break;
        }
    } else if (quiet->count > 0) {
        log_level = LOG_ERROR;
    }

    info.log_level = log_level;
    info.verbosity = verbose->count > 0 ? OUT_VERBOSE : OUT_NORMAL;
    info.quiet = quiet->count > 0;

    log_set_level(info.log_level);
    log_set_verbosity(info.verbosity);
    log_set_quiet(info.quiet);

    argv = uv_setup_args(argc, argv);
    arg_set_module_name(APP_NAME);

    const char* subcmd = argv[subcmd_index];
    int subcmd_argc = argc - subcmd_index;
    char** subcmd_argv = argv + subcmd_index;

    arg_cmd_itr_t itr = arg_cmd_itr_create();
    if (arg_cmd_itr_search(itr, (void*)subcmd) == 0) {
        out_error("unknown subcommand: %s", subcmd);
        arg_dstr_destroy(res);
        arg_cmd_itr_destroy(itr);
        arg_cmd_uninit();
        return 1;
    }
    arg_cmd_itr_destroy(itr);

    rv = arg_cmd_dispatch(subcmd, subcmd_argc, subcmd_argv, res);

    const char* msg = arg_dstr_cstr(res);
    if (strlen(msg) > 0) {
        out_data("%s\n", msg);
    }

    arg_dstr_destroy(res);
    arg_cmd_uninit();
    return rv;
}
