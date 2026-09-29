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

#include "cmd_bm_show.h"

#include "common/hes_gateway.h"
#include "common/hes_query.h"

#include <argtable3.h>
#include <log.h>

#include <stdlib.h>
#include <string.h>

////////////////////////////////////////////////////////////////////////////////
/// Short alias for hes_query_string() — readability only.
static inline const char* qstr(hes_query_ctx_t* ctx, const char* path)
{
    return hes_query_string(ctx, path);
}

////////////////////////////////////////////////////////////////////////////////
/// Loads a binding-map document into the native structs and prints the parts
/// this reading test cares about: the <file> header fields and the first
/// lexicon's object types. It exercises the XML layer (hes_gateway.h), not the
/// gateway's runtime path -- see cmd_bm_show_proc().
///
/// @param filepath The binding-map XML instance document to load.
static void test_hes_gateway_api(const char* filepath)
{
    hes_gateway_t* gw = hes_gateway_load(filepath);
    if (gw == NULL) {
        log_error("failed to load gateway struct");
        return;
    }

    out_info("overallProcess version  = %s", gw->overall_process->version_number);
    out_info("Gateway product: %s / %s (version %s)", gw->file_manu, gw->file_product,
             gw->file_version);
    out_info("Created: %s  Revised: %s", gw->file_created_date, gw->file_revision_date);

    if (gw->overall_process == NULL || gw->overall_process->lexicon_type_count == 0 ||
        gw->overall_process->lexicon_types[0]->object_type_count == 0) {
        log_warn("empty gateway structure");
        hes_gateway_free(gw);
        return;
    }

    hes_gw_object_type_t* obj = gw->overall_process->lexicon_types[0]->object_types[0];
    out_info("[0] objectType: transCode = %s, descriptiveName = %s", obj->trans_code,
             obj->descriptive_name);

    out_info("\n--- Operation Tables (%zu) ---\n", obj->operation_table_count);
    for (size_t i = 0; i < obj->operation_table_count; i++) {
        hes_gw_operation_table_t* ot = obj->operation_tables[i];

        const char* ref_id = hes_gw_data_value(ot->data, "refId");
        const char* enable = hes_gw_data_value(ot->data, "enable");
        const char* op = hes_gw_data_value(ot->data, "operation");
        out_info("  [%zu] %s refId = %s, enable = %s, operation = %s, inputs count = %zu", i,
                 ot->trans_code, ref_id ? ref_id : "", enable ? enable : "", op ? op : "",
                 ot->inputs_count);

        for (size_t j = 0; j < ot->inputs_count; j++) {
            hes_gw_inputs_t* in = ot->inputs[j];
            const char* src = hes_gw_data_value(in->data, "sourceObject");
            const char* dev = hes_gw_data_value(in->data, "deviceIndex");
            out_info("    inputs[%zu] %s deviceIndex (di) = %s, sourceObject (so) = %s", j,
                     in->trans_code, dev ? dev : "", src ? src : "");
        }

        for (size_t j = 0; j < ot->input_parameters_count; j++) {
            hes_gw_param_group_t* ip = ot->input_parameters[j];
            const char* param = hes_gw_data_value(ip->data, "inputParameter");
            out_info("    inputParameters[%zu] %s inputParameter (ip) = %s", j, ip->trans_code,
                     param ? param : "");
        }

        for (size_t j = 0; j < ot->output_parameters_count; j++) {
            hes_gw_param_group_t* opg = ot->output_parameters[j];
            const char* param = hes_gw_data_value(opg->data, "outputParameter");
            out_info("    outputParameters[%zu] %s outputParameter (op) = %s", j, opg->trans_code,
                     param ? param : "");
        }

        for (size_t j = 0; j < ot->outputs_count; j++) {
            hes_gw_outputs_t* out = ot->outputs[j];
            const char* dl = hes_gw_data_value(out->data, "deviceList");
            const char* dest = hes_gw_data_value(out->data, "destObject");
            out_info("    outputs[%zu] %s deviceList (dl) = %s, destObject (do) = %s", j,
                     out->trans_code, dl ? dl : "", dest ? dest : "");
        }
    }

    out_info("\n--- Addressing Tables (%zu) ---\n", obj->addressing_table_count);
    for (size_t i = 0; i < obj->addressing_table_count; i++) {
        hes_gw_addressing_table_t* at = obj->addressing_tables[i];

        const char* dev = hes_gw_data_value(at->data, "deviceIndex");
        const char* mod = hes_gw_data_value(at->data, "moduleType");
        const char* mod_ref = hes_gw_data_value(at->data, "moduleRefIndex");
        const char* net = hes_gw_data_value(at->data, "netRefIndex");
        out_info(
                "  [%zu] %s deviceIndex (di) = %s, moduleType (mt) = %s, moduleRefIndex (mi) = %s, "
                "netRefIndex (ni) = %s",
                i, at->trans_code, dev ? dev : "", mod ? mod : "", mod_ref ? mod_ref : "",
                net ? net : "");
    }

    hes_gateway_free(gw);
}

const char* cmd_bm_show_name()
{
    static char name[] = "show";
    return name;
}

const char* cmd_bm_show_desc()
{
    static char description[] = "output binding map information";
    return description;
}

int cmd_bm_show_proc(int argc, char* argv[], arg_dstr_t res, void* ctx)
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
    if (arg_make_syntax_err_help_msg(res, cmd_bm_show_name(), help->count, nerrors, argtable, end,
                                     &exitcode)) {
        goto exit;
    }

    //////////////////////////////////////////////////////////////////////////////
    // process the command

    if (cmd_name->count == 0) {
        exitcode = 1;
        goto exit;
        log_error("Usage: %s <binding_map.xml>", argv[0]);
        return EXIT_FAILURE;
    }

    test_hes_gateway_api(cmd_name->sval[0]);

exit:
    arg_freetable(argtable, sizeof(argtable) / sizeof(argtable[0]));
    return exitcode;
}
