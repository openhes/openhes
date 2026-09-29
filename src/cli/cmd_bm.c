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

#include "cmd_bm.h"

#include "cmd_bm_show.h"

#include <argtable3.h>
#include <log.h>

#include <stdlib.h>
#include <string.h>

const char* cmd_bm_name()
{
    static char name[] = "bm";
    return name;
}

const char* cmd_bm_desc()
{
    static char description[] = "launch binding map commands";
    return description;
}

int cmd_bm_proc(int argc, char* argv[], arg_dstr_t res, void* ctx)
{
    if (argc == 1) {
        arg_make_get_help_msg(res);
        return 1;
    }

    arg_cmd_register(cmd_bm_show_name(), cmd_bm_show_proc, cmd_bm_show_desc(), ctx);
    return arg_cmd_dispatch(argv[1], argc - 1, argv + 1, res);
}
