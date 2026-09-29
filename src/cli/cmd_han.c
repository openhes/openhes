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

#include "cmd_han.h"

#include "cmd_han_ble.h"
#include "cmd_han_wifi.h"

#include <argtable3.h>
#include <log.h>

#include <stdlib.h>
#include <string.h>

const char* cmd_han_name()
{
    static char name[] = "han";
    return name;
}

const char* cmd_han_desc()
{
    static char description[] = "launch han interface module commands";
    return description;
}

int cmd_han_proc(int argc, char* argv[], arg_dstr_t res, void* ctx)
{
    if (argc == 1) {
        arg_make_get_help_msg(res);
        return 1;
    }

    arg_cmd_register(cmd_han_ble_name(), cmd_han_ble_proc, cmd_han_ble_desc(), ctx);
    arg_cmd_register(cmd_han_wifi_name(), cmd_han_wifi_proc, cmd_han_wifi_desc(), ctx);
    return arg_cmd_dispatch(argv[1], argc - 1, argv + 1, res);
}
