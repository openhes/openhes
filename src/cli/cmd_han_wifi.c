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

#include "cmd_han_wifi.h"

#include "app_config.h"
#include "han/wifi/wifi.h"

#include <argtable3.h>
#include <log.h>

#include <stdlib.h>
#include <string.h>

#define DEFAULT_BULB_IP "192.168.1.142"

const char* cmd_han_wifi_name()
{
    static char name[] = "wifi";
    return name;
}

const char* cmd_han_wifi_desc()
{
    static char description[] = "launch WiFi interface module";
    return description;
}

int cmd_han_wifi_proc(int argc, char* argv[], arg_dstr_t res, void* ctx)
{
    struct arg_str* cmd_name = arg_str0(NULL, NULL, "<command>", NULL);
    arg_str_t* svc_rep =
            arg_str0(NULL, "svc-rep", "<url>",
                     "manifest service REQ/REP URL (default " HES_MANIFEST_REP_URL ")");
    arg_str_t* svc_pub = arg_str0(NULL, "svc-pub", "<url>",
                                  "manifest service PUB URL (default " HES_MANIFEST_PUB_URL ")");
    arg_str_t* module_ref =
            arg_str0(NULL, "module-ref", "<index>", "this module's identity (default 2)");
    arg_str_t* hub_pub_url =
            arg_str0(NULL, "hub-pub", "<url>",
                     "hub PUB URL, this module SUBs to (default " HES_HUB_PUB_URL ")");
    arg_str_t* hub_sub_url =
            arg_str0(NULL, "hub-sub", "<url>",
                     "hub SUB URL, this module PUBs to (default " HES_HUB_SUB_URL ")");
    arg_str_t* bulb_ip =
            arg_str0(NULL, "bulb-ip", "<ip>", "WiZ bulb IP address (default 192.168.1.142)");
    struct arg_lit* help = arg_lit0("h", "help", "output usage information");
    struct arg_end* end = arg_end(20);
    void* argtable[] = {cmd_name,    svc_rep, svc_pub, module_ref, hub_pub_url,
                        hub_sub_url, bulb_ip, help,    end};

    int exitcode = 0;
    if (arg_nullcheck(argtable) != 0) {
        log_error("fail to allocate argtable");
        exitcode = 1;
        goto exit;
    }

    svc_rep->sval[0] = HES_MANIFEST_REP_URL;
    svc_pub->sval[0] = HES_MANIFEST_PUB_URL;
    module_ref->sval[0] = "2";
    hub_pub_url->sval[0] = HES_HUB_PUB_URL;
    hub_sub_url->sval[0] = HES_HUB_SUB_URL;
    bulb_ip->sval[0] = DEFAULT_BULB_IP;

    int nerrors = arg_parse(argc, argv, argtable);
    if (arg_make_syntax_err_help_msg(res, cmd_han_wifi_name(), help->count, nerrors, argtable, end,
                                     &exitcode)) {
        goto exit;
    }

    //////////////////////////////////////////////////////////////////////////////
    // process the command

    exitcode = han_wifi_main(svc_rep->sval[0], svc_pub->sval[0],
                             (uint32_t)strtoul(module_ref->sval[0], NULL, 10), hub_pub_url->sval[0],
                             hub_sub_url->sval[0], bulb_ip->sval[0]);

exit:
    arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
    return exitcode;
}
