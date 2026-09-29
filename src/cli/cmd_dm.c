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

#include "cmd_dm.h"

#include "dm/dm.h"
#include "utils.h"

#include <argtable3.h>
#include <log.h>
#include <miniz.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

const char* cmd_dm_name()
{
    static char name[] = "dm";
    return name;
}

const char* cmd_dm_desc()
{
    static char description[] = "launch device manifest service";
    return description;
}

int cmd_dm_proc(int argc, char* argv[], arg_dstr_t res, void* ctx)
{
    struct arg_str* cmd_name = arg_str0(NULL, NULL, "<command>", NULL);
    arg_str_t* profile = arg_str0("p", "profile", "<file>", "product profile json file");
    arg_str_t* rep_url = arg_str0(NULL, "rep", "<url>", "req/rep listen url");
    arg_str_t* pub_url = arg_str0(NULL, "pub", "<url>", "pub notify url");
    struct arg_lit* help = arg_lit0("h", "help", "output usage information");
    struct arg_end* end = arg_end(20);
    void* argtable[] = {cmd_name, profile, rep_url, pub_url, help, end};

    int exitcode = 0;
    if (arg_nullcheck(argtable) != 0) {
        log_error("fail to allocate argtable");
        exitcode = 1;
        goto exit;
    }

    app_info_t* info = (app_info_t*)ctx;
    profile->sval[0] = info->profile_path;
    rep_url->sval[0] = info->default_rep_url;
    pub_url->sval[0] = info->default_pub_url;

    int nerrors = arg_parse(argc, argv, argtable);
    if (arg_make_syntax_err_help_msg(res, cmd_dm_name(), help->count, nerrors, argtable, end,
                                     &exitcode)) {
        goto exit;
    }

    //////////////////////////////////////////////////////////////////////////////
    // process the command

    exitcode = manifest_service_main(profile->sval[0], rep_url->sval[0], pub_url->sval[0]);

exit:
    arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
    return exitcode;
}
