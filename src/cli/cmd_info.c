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

#define _GNU_SOURCE

#include "cmd_info.h"

#include "utils.h"

#include <log.h>
#include <miniz.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

const char* cmd_info_name()
{
    static char name[] = "info";
    return name;
}

const char* cmd_info_desc()
{
    static char description[] = "output system information";
    return description;
}

int cmd_info_proc(int argc, char* argv[], arg_dstr_t res, void* ctx)
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
    if (arg_make_syntax_err_help_msg(res, cmd_info_name(), help->count, nerrors, argtable, end,
                                     &exitcode)) {
        goto exit;
    }

    //////////////////////////////////////////////////////////////////////////////
    // process the command

    app_info_t* info = (app_info_t*)ctx;
    arg_dstr_catf(res, "exe_path        = \"%s\"\n", info->exe_path);
    arg_dstr_catf(res, "home            = \"%s\"\n", info->home);
    arg_dstr_catf(res, "cwd             = \"%s\"\n", info->cwd);
    arg_dstr_catf(res, "tmp_dir         = \"%s\"\n", info->tmp_dir);
    arg_dstr_catf(res, "data_home       = \"%s\"\n", info->data_home);
    arg_dstr_catf(res, "state_home      = \"%s\"\n", info->state_home);
    arg_dstr_catf(res, "cache_home      = \"%s\"\n", info->cache_home);
    arg_dstr_catf(res, "config_dir      = \"%s\"\n", info->config_dir);
    arg_dstr_catf(res, "config_path     = \"%s\"\n", info->config_path);
    arg_dstr_catf(res, "profile_path    = \"%s\"\n", info->profile_path);
    arg_dstr_catf(res, "default_rep_url = \"%s\"\n", info->default_rep_url);
    arg_dstr_catf(res, "default_pub_url = \"%s\"\n", info->default_pub_url);

exit:
    arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
    return exitcode;
}
