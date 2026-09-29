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

#include "cmd_sm_core.h"

#include "app_config.h"
#include "modules/core/core.h"

#include <argtable3.h>
#include <log.h>

#include <stdlib.h>
#include <string.h>

const char* cmd_sm_core_name()
{
    static char name[] = "core";
    return name;
}

const char* cmd_sm_core_desc()
{
    static char description[] = "launch core service module";
    return description;
}

int cmd_sm_core_proc(int argc, char* argv[], arg_dstr_t res, void* ctx)
{
    struct arg_str* cmd_name = arg_str0(NULL, NULL, "<command>", NULL);
    struct arg_str* pub_url =
            arg_str0(NULL, "pub-url", "<url>",
                     "pub URL for downstream communication (default " HES_HUB_PUB_URL ")");
    struct arg_str* sub_url =
            arg_str0(NULL, "sub-url", "<url>",
                     "sub URL for upstream communication (default " HES_HUB_SUB_URL ")");
    struct arg_str* bm_xml = arg_str0(NULL, "bm-xml", "<file>", "specify the BM XML file");
    struct arg_str* lua_app = arg_str0(NULL, "app", "<file>", "specify the Lua application file");
    struct arg_str* auth_policy = arg_str0(NULL, "auth-policy", "<file>",
                                           "authorization policy file (read-only, secret)");
    struct arg_str* identity =
            arg_str0(NULL, "identity", "<file>",
                     "gateway identity document (identification service, secret)");
    struct arg_str* svc_rep = arg_str0(
            NULL, "svc-rep", "<url>",
            "manifest service REQ URL, for the gateway's module list (default " HES_MANIFEST_REP_URL
            ")");
    struct arg_str* ntp_servers =
            arg_str0(NULL, "ntp-servers", "<list>", "NTP hosts to sync from, comma separated");
    struct arg_lit* no_ntp = arg_lit0(NULL, "no-ntp", "never contact an NTP server");
    struct arg_lit* no_authz =
            arg_lit0(NULL, "no-authz",
                     "ignore every authorization gate: 'at' and 'ap=au' (development switch)");
    struct arg_str* time_zone = arg_str0(NULL, "time-zone", "<zone>",
                                         "force an IANA zone (e.g. Asia/Taipei); skips detection");
    struct arg_str* tz_urls =
            arg_str0(NULL, "tz-urls", "<list>", "time zone lookup endpoints, comma separated");
    struct arg_lit* no_tz = arg_lit0(NULL, "no-tz", "never contact a time zone lookup service");
    struct arg_lit* help = arg_lit0("h", "help", "output usage information");
    struct arg_end* end = arg_end(20);
    void* argtable[] = {cmd_name,  pub_url, sub_url, bm_xml,      lua_app, auth_policy,
                        identity,  svc_rep, help,    ntp_servers, no_ntp,  no_authz,
                        time_zone, tz_urls, no_tz,   end};

    int exitcode = 0;
    if (arg_nullcheck(argtable) != 0) {
        log_error("fail to allocate argtable");
        exitcode = 1;
        goto exit;
    }

    pub_url->sval[0] = HES_HUB_PUB_URL;
    sub_url->sval[0] = HES_HUB_SUB_URL;
    svc_rep->sval[0] = HES_MANIFEST_REP_URL;

    int nerrors = arg_parse(argc, argv, argtable);
    if (arg_make_syntax_err_help_msg(res, cmd_sm_core_name(), help->count, nerrors, argtable, end,
                                     &exitcode)) {
        goto exit;
    }

    //////////////////////////////////////////////////////////////////////////////
    // process the command

    core_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pub_url = pub_url->sval[0];
    cfg.sub_url = sub_url->sval[0];
    cfg.bm_xml = bm_xml->sval[0];
    cfg.lua_app = lua_app->sval[0];
    // argtable hands an option that was not given an empty string, not NULL, so
    // "no policy" would otherwise reach the service as the unopenable file "".
    cfg.auth_policy = (auth_policy->sval[0][0] != '\0') ? auth_policy->sval[0] : NULL;
    cfg.identity_path = identity->sval[0];
    cfg.manifest_rep_url = svc_rep->sval[0];
    cfg.ntp_servers = ntp_servers->sval[0];
    cfg.tz_name = time_zone->sval[0];
    cfg.tz_urls = tz_urls->sval[0];
    cfg.ntp_enabled = (no_ntp->count == 0);
    cfg.tz_enabled = (no_tz->count == 0);
    cfg.no_authz = (no_authz->count > 0);

    out_data("core service module: starting with command '%s'\n", cmd_name->sval[0]);
    out_data("core service module: using pub URL '%s'\n", pub_url->sval[0]);
    out_data("core service module: using sub URL '%s'\n", sub_url->sval[0]);
    out_data("core service module: using BM XML file '%s'\n", bm_xml->sval[0]);
    out_data("core service module: using Lua application file '%s'\n", lua_app->sval[0]);
    out_data("core service module: using auth policy file '%s'\n",
             cfg.auth_policy ? cfg.auth_policy : "(none)");
    out_data("core service module: authorization %s\n",
             cfg.no_authz ? "DISABLED (--no-authz: 'at' and 'ap=au' gates ignored)" : "enabled");
    out_data("core service module: using identity document '%s'\n", identity->sval[0]);
    out_data("core service module: module list from manifest service '%s'\n",
             cfg.manifest_rep_url ? cfg.manifest_rep_url : "(none: addressing table fallback)");
    out_data("core service module: NTP %s%s\n", cfg.ntp_enabled ? "enabled" : "disabled",
             cfg.ntp_servers ? cfg.ntp_servers : " (built-in servers)");
    out_data("core service module: time zone %s\n",
             cfg.tz_name ? cfg.tz_name : (cfg.tz_enabled ? "auto-detect" : "from the host"));

    exitcode = core_main(&cfg);

exit:
    arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
    return exitcode;
}
