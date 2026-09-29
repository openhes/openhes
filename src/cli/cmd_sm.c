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

#include "cmd_sm.h"

#include "cmd_sm_core.h"

#include <argtable3.h>
#include <log.h>

#include <stdlib.h>
#include <string.h>

const char* cmd_sm_name()
{
    static char name[] = "sm";
    return name;
}

const char* cmd_sm_desc()
{
    static char description[] = "launch service module commands";
    return description;
}

int cmd_sm_proc(int argc, char* argv[], arg_dstr_t res, void* ctx)
{
    if (argc == 1) {
        arg_make_get_help_msg(res);
        return 1;
    }

    arg_cmd_register(cmd_sm_core_name(), cmd_sm_core_proc, cmd_sm_core_desc(), ctx);
    return arg_cmd_dispatch(argv[1], argc - 1, argv + 1, res);
}
