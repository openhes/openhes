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

#include "cmd_help.h"
#include "app_config.h"
#include "app_version.h"

#include <argtable3.h>
#include <log.h>

#include <stdlib.h>
#include <string.h>

typedef struct cmd_name {
    char name[ARG_CMD_NAME_LEN];
} cmd_name_t;

////////////////////////////////////////////////////////////////////////////////
/// qsort() comparator for the command table: compares two names as strings, so
/// the list of commands is presented in alphabetical order.
///
/// @param str1 The first name, cast to `const char *`.
/// @param str2 The second name.
/// @return Negative, zero or positive, as strcmp() returns.
static int compare_str(const void* str1, const void* str2)
{
    return strcmp((const char*)str1, (const char*)str2);
}

const char* cmd_help_name()
{
    static char name[] = "help";
    return name;
}

const char* cmd_help_desc()
{
    static char description[] = "output usage information";
    return description;
}

int cmd_help_proc(int argc, char* argv[], arg_dstr_t res, void* ctx)
{
    struct arg_str* cmd_name = arg_str0(NULL, NULL, "<command>", NULL);
    struct arg_lit* help = arg_lit0("h", "help", "output usage information");
    struct arg_end* end = arg_end(20);
    void* argtable[] = {cmd_name, help, end};

    int exitcode = 0;
    if (arg_nullcheck(argtable) != 0) {
        log_error("fail to allocate argtable");
        exitcode = 1;
        goto exit;
    }

    int nerrors = arg_parse(argc, argv, argtable);
    if (arg_make_syntax_err_help_msg(res, cmd_help_name(), help->count, nerrors, argtable, end,
                                     &exitcode)) {
        goto exit;
    }

    //////////////////////////////////////////////////////////////////////////////
    // process the command

    if (cmd_name->count == 0) {
        arg_dstr_cat(res, "Usage:\n");
        arg_dstr_catf(res, "  %s [global options] <command> [options] [args]\n\n", APP_NAME);
        arg_dstr_cat(res, "Available commands:\n");

        cmd_name_t* acmd = (cmd_name_t*)malloc(arg_cmd_count() * sizeof(cmd_name_t));
        size_t max_cmd_name_length = 0;
        arg_cmd_itr_t itr = arg_cmd_itr_create();
        int i = 0;
        do {
            arg_cmd_info_t* cmd_info = arg_cmd_itr_value(itr);
            snprintf(acmd[i].name, sizeof(acmd[i].name), "%s", cmd_info->name);
            i++;

            if (strlen(cmd_info->name) > max_cmd_name_length) {
                max_cmd_name_length = strlen(cmd_info->name);
            }
        } while (arg_cmd_itr_advance(itr));
        arg_cmd_itr_destroy(itr);

        arg_mgsort(acmd, arg_cmd_count(), sizeof(cmd_name_t), 0, arg_cmd_count() - 1, compare_str);
        for (i = 0; i < arg_cmd_count(); i++) {
            arg_cmd_info_t* cmd_info = arg_cmd_info(acmd[i].name);
            arg_dstr_catf(res, "  %-*s  %s\n", max_cmd_name_length, cmd_info->name,
                          cmd_info->description);
        }
        free(acmd);

        arg_dstr_catf(res, "\nType \"%s help <command>\" for help on a specific command.\n",
                      APP_NAME);
    } else {
        if (arg_cmd_info(cmd_name->sval[0]) == NULL) {
            arg_dstr_catf(res, "Unknown command: %s\n", cmd_name->sval[0]);
            arg_make_get_help_msg(res);
            exitcode = 1;
            goto exit;
        }

        arg_cmd_info_t* cmd_info = arg_cmd_info(cmd_name->sval[0]);
        int tmp_argc = 3;
        char* tmp_argv[] = {"", cmd_info->name, "--help"};
        cmd_info->proc(tmp_argc, tmp_argv, res, ctx);
    }

exit:
    arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
    return exitcode;
}
