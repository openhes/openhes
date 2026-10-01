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
/// @brief Binding map service: controller + processor implementation
/// (XML parsing, operation-table evaluation, HES-CLME subscribe/put dispatch).
///
/// @details
/// Implementation half of the binding map declared in bm.h (see there for the
/// ISO/IEC 18012-3 / 15045-4-1 background). This translation unit provides:
///
///   - XML parsing: bm_load_xml() reads a binding-map instance document through
///     common/hes_query.h (the tree's only XML layer) and the parse_*() helpers
///     turn it into the operation/addressing tables.
///
///   - "controller" service: bm_controller_start() sends one SUBSCRIBE per
///     distinct (path, deviceIndex) whose addressing-table owner is an external
///     (hi/wi/sm) module -- never for 'it'-chained internal values.
///
///   - "processor" service: bm_processor_handle() keeps a value cache keyed by
///     deviceIndex and calls bm_evaluate_all(); evaluate_op() fires each
///     operation whose inputs are all valid, caches the result at its output
///     device, and emits a PUT only when the result actually changed
///     (has_last_output change detection -- unchanged values never flood the
///     bus). Firing one op may unlock downstream 'it'-chained rows, so
///     bm_evaluate_all() recurses; the change-detection bound keeps that
///     termination-safe for the acyclic (DAG-shaped) tables the standard
///     describes.
///
/// Memory/ownership: all persistent state lives in the caller-supplied
/// binding_map_t (bm_init() zeroes it, bm_load_xml() fills it); this unit owns
/// no heap beyond the transient query context, which is closed before
/// bm_load_xml() returns.
///
/// Threading: not internally synchronized. bm_controller_start() and
/// bm_processor_handle() are intended to be driven from one event loop (see the
/// core service module, src/modules/core/core.c).

#include "bm.h"

#include "common/hes_query.h"

#include <log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// The operation table's inputParameter code for "the value of currentValue"
/// (18012-3 Table 26). It is the datum a service object's answer carries under
/// the transCode 'va', which is the one an operation reads live.
#define BM_LIVE_INPUT_PARAM "va"

////////////////////////////////////////////////////////////////////////////////
/// Copies an element's text into a caller buffer. hes_query hands out a string
/// borrowed from the document, so it is copied out here; a value longer than the
/// buffer is truncated by hes_strlcpy, as before.
static void elem_text_copy(hes_query_elem_t* elem, char* out, size_t outsz)
{
    const char* text = hes_query_elem_text(elem);
    if (text != NULL) {
        hes_strlcpy(out, outsz, text);
    } else {
        out[0] = '\0';
    }
}

////////////////////////////////////////////////////////////////////////////////
/// Maps the text of an addressing-table 'mt' datum to its enum value: "hi" HAN,
/// "wi" WAN, "sm" service module, "it" internal process. Anything else --
/// including an empty value -- is MODTYPE_NONE, which the callers read as "no
/// external module to talk to".
///
/// @param s The 'mt' text, e.g. "hi".
/// @return The matching hes_module_type_t, or HES_MODTYPE_NONE.
static hes_module_type_t parse_module_type(const char* s)
{
    if (!strcmp(s, "hi")) {
        return HES_MODTYPE_HI;
    } else if (!strcmp(s, "wi")) {
        return HES_MODTYPE_WI;
    } else if (!strcmp(s, "sm")) {
        return HES_MODTYPE_SM;
    } else if (!strcmp(s, "it")) {
        return HES_MODTYPE_IT;
    } else {
        return HES_MODTYPE_NONE;
    }
}

////////////////////////////////////////////////////////////////////////////////
/// Fills one addressing-table row from one <addressingTable> element: 'di'
/// (deviceIndex), 'mt' (moduleType), 'mi' (moduleRefIndex), 'ni'
/// (netRefIndex) and 'ad' (the module's address).
///
/// This row is what says which module owns a device, so it is also what makes
/// a command to that device relayable -- see bm_is_declared_destination().
///
/// A row beyond BM_MAX_ADDRS is dropped silently, like the other parsers.
///
/// @param atNode The <addressingTable> element to read.
/// @param bm     The binding map to add the row to.
static void parse_addressing_table(hes_query_elem_t* atNode, binding_map_t* bm)
{
    if (bm->n_addrs >= BM_MAX_ADDRS) {
        return;
    }

    bm_addr_t* a = &bm->addrs[bm->n_addrs++];
    memset(a, 0, sizeof(*a));

    size_t n = 0;
    hes_query_elem_t** data = hes_query_elem_children(atNode, "data", &n);
    for (size_t i = 0; i < n; i++) {
        const char* tc = hes_query_elem_attr(data[i], "transCode");
        if (tc == NULL) {
            continue;
        }

        char buf[128] = {0};
        elem_text_copy(data[i], buf, sizeof(buf));

        if (strcmp(tc, "di") == 0) {
            a->device_index = (uint32_t)atoi(buf);
        } else if (strcmp(tc, "mt") == 0) {
            a->module_type = parse_module_type(buf);
        } else if (strcmp(tc, "mi") == 0) {
            a->module_ref_index = (uint32_t)atoi(buf);
        } else if (strcmp(tc, "ni") == 0) {
            a->net_ref_index = (uint32_t)atoi(buf);
        } else if (strcmp(tc, "ad") == 0) {
            hes_strlcpy(a->address, sizeof(a->address), buf);
        }
    }

    hes_query_elem_list_free(data);
}

////////////////////////////////////////////////////////////////////////////////
/// Fills one <inputs> entry of an operation row: 'di' (the source device), 'so'
/// (the source object path), 'co' (conditioning), 'at' (input authorType) and
/// 'ap' (accompanyOperation). These are the operands the row evaluates, in
/// document order; a row may hold more than one.
///
/// An entry beyond BM_MAX_INPUTS is dropped silently.
///
/// @param inNode The <inputs> element to read.
/// @param op     The row to add the input to.
static void parse_inputs(hes_query_elem_t* inNode, bm_operation_t* op)
{
    if (op->n_inputs >= BM_MAX_INPUTS) {
        return;
    }

    bm_input_t* in = &op->inputs[op->n_inputs++];
    memset(in, 0, sizeof(*in));

    size_t n = 0;
    hes_query_elem_t** data = hes_query_elem_children(inNode, "data", &n);
    for (size_t i = 0; i < n; i++) {
        const char* tc = hes_query_elem_attr(data[i], "transCode");
        if (tc == NULL) {
            continue;
        }

        char buf[HES_PATH_MAX] = {0};
        elem_text_copy(data[i], buf, sizeof(buf));

        if (strcmp(tc, "di") == 0) {
            in->device_index = (uint32_t)atoi(buf);
        } else if (strcmp(tc, "so") == 0) {
            hes_strlcpy(in->source_object, sizeof(in->source_object), buf);
        } else if (strcmp(tc, "co") == 0) {
            hes_strlcpy(in->condition, sizeof(in->condition), buf);
        } else if (strcmp(tc, "at") == 0) {
            hes_strlcpy(in->author_type, sizeof(in->author_type), buf);
        } else if (strcmp(tc, "ap") == 0) {
            hes_strlcpy(in->accompany_op, sizeof(in->accompany_op), buf);
        }

        // 'sv', 'me' and the accompanied credential fields ('ci','un') are not
        // needed for the dataflow graph: the credential travels on the message,
        // not in the XML. See src/services/auth/README.md.
    }

    hes_query_elem_list_free(data);
}

////////////////////////////////////////////////////////////////////////////////
/// Fills the row's inputParameters: one number per entry, or the sentinel that
/// means "use the live value of this input's object" (the XML writes the
/// literal "va" for it -- see BM_LIVE_INPUT_PARAM and ip_is_live[]).
///
/// At most BM_MAX_INPUTS entries; the rest are ignored.
///
/// @param ipNode The <inputParameters> element to read.
/// @param op     The row to add the parameters to.
static void parse_input_parameters(hes_query_elem_t* ipNode, bm_operation_t* op)
{
    size_t n = 0;
    hes_query_elem_t** data = hes_query_elem_children(ipNode, "data", &n);
    for (size_t i = 0; i < n; i++) {
        const char* tc = hes_query_elem_attr(data[i], "transCode");
        if (tc == NULL || strcmp(tc, "ip") != 0 || op->n_input_params >= BM_MAX_INPUTS) {
            continue;
        }

        char buf[64] = {0};
        elem_text_copy(data[i], buf, sizeof(buf));

        int idx = op->n_input_params++;
        if (strcmp(buf, BM_LIVE_INPUT_PARAM) == 0) {
            op->ip_is_live[idx] = 1;
            op->input_params[idx] = 0.0;
        } else {
            op->ip_is_live[idx] = 0;
            op->input_params[idx] = atof(buf);
        }
    }

    hes_query_elem_list_free(data);
}

////////////////////////////////////////////////////////////////////////////////
/// Fills the row's outputParameters: which branch of the operation maps to which
/// output. This POC keeps a single boolean result, so at most two entries are
/// read into output_params[] (see bm.h).
///
/// @param opNode The <outputParameters> element to read.
/// @param op     The row to add the parameters to.
static void parse_output_parameters(hes_query_elem_t* opNode, bm_operation_t* op)
{
    size_t n = 0;
    hes_query_elem_t** data = hes_query_elem_children(opNode, "data", &n);
    for (size_t i = 0; i < n; i++) {
        const char* tc = hes_query_elem_attr(data[i], "transCode");
        if (tc == NULL || strcmp(tc, "op") != 0 || op->n_output_params >= 2) {
            continue;
        }

        char buf[64] = {0};
        elem_text_copy(data[i], buf, sizeof(buf));

        op->output_params[op->n_output_params++] = atoi(buf);
    }

    hes_query_elem_list_free(data);
}

////////////////////////////////////////////////////////////////////////////////
/// Fills one <outputs> entry of an operation row: the element's own transCode
/// into out_code (it forms part of the row's authorType address), plus 'dl'
/// (the output device), 'do' (the destination object), 'me' (method, e.g. "up"),
/// 'ad' (adjustment) and 'at' (output authorType).
///
/// @param outNode The <outputs> element to read.
/// @param op      The row to add the output to.
static void parse_outputs(hes_query_elem_t* outNode, bm_operation_t* op)
{
    const char* code = hes_query_elem_attr(outNode, "transCode");
    if (code != NULL) {
        hes_strlcpy(op->out_code, sizeof(op->out_code), code);
    }

    size_t n = 0;
    hes_query_elem_t** data = hes_query_elem_children(outNode, "data", &n);
    for (size_t i = 0; i < n; i++) {
        const char* tc = hes_query_elem_attr(data[i], "transCode");
        if (tc == NULL) {
            continue;
        }

        char buf[HES_PATH_MAX] = {0};
        elem_text_copy(data[i], buf, sizeof(buf));

        if (strcmp(tc, "dl") == 0) {
            op->out_device_index = (uint32_t)atoi(buf);
        } else if (strcmp(tc, "do") == 0) {
            hes_strlcpy(op->out_dest_object, sizeof(op->out_dest_object), buf);
        } else if (strcmp(tc, "me") == 0) {
            hes_strlcpy(op->out_method, sizeof(op->out_method), buf);
        } else if (strcmp(tc, "ad") == 0) {
            hes_strlcpy(op->out_adjustment, sizeof(op->out_adjustment), buf);
        } else if (strcmp(tc, "at") == 0) {
            hes_strlcpy(op->out_author_type, sizeof(op->out_author_type), buf);
        }

        // 'dv' and the outputs-context 'op' (optinOut) are part of the full
        // model (privacy opt-in/out) and are not used by this POC's evaluation
        // logic yet.
    }

    hes_query_elem_list_free(data);
}

////////////////////////////////////////////////////////////////////////////////
/// Runs `loader` on every child of `parent` named `name`, in document order, and
/// drops the handles again. An operation row's containers write to separate
/// fields, so reading them one name at a time is equivalent to one pass.
static void load_group(hes_query_elem_t* parent,
                       const char* name,
                       void (*loader)(hes_query_elem_t*, bm_operation_t*),
                       bm_operation_t* op)
{
    size_t n = 0;
    hes_query_elem_t** group = hes_query_elem_children(parent, name, &n);
    for (size_t i = 0; i < n; i++) {
        loader(group[i], op);
    }

    hes_query_elem_list_free(group);
}

////////////////////////////////////////////////////////////////////////////////
/// Fills one operation row from one <operationTable> element: first the row's own
/// fields ('ri' refId, 'en' enable, 'op' operation code), then its four
/// container groups (inputs, inputParameters, outputParameters, outputs).
///
/// A row is enabled unless it says otherwise: the 'en' datum must read "en" to
/// enable it, and a row with no 'en' datum at all stays enabled, because the
/// element being present is the intent to use it.
///
/// A row beyond BM_MAX_OPS is dropped silently.
///
/// @param otNode The <operationTable> element to read.
/// @param bm     The binding map to add the row to.
static void parse_operation_table(hes_query_elem_t* otNode, binding_map_t* bm)
{
    if (bm->n_ops >= BM_MAX_OPS) {
        return;
    }

    bm_operation_t* op = &bm->ops[bm->n_ops++];
    memset(op, 0, sizeof(*op));
    op->enabled = 1;

    // The element's own transCode (e.g. "ot1") is part of this row's
    // authorType address -- see bm_at_address().
    const char* code = hes_query_elem_attr(otNode, "transCode");
    if (code != NULL) {
        hes_strlcpy(op->table_code, sizeof(op->table_code), code);
    }

    // the row's own fields
    size_t n = 0;
    hes_query_elem_t** data = hes_query_elem_children(otNode, "data", &n);
    for (size_t i = 0; i < n; i++) {
        const char* tc = hes_query_elem_attr(data[i], "transCode");
        if (tc == NULL) {
            continue;
        }

        char buf[64] = {0};
        elem_text_copy(data[i], buf, sizeof(buf));

        if (strcmp(tc, "ri") == 0) {
            op->ref_id = (uint32_t)atoi(buf);
        } else if (strcmp(tc, "en") == 0) {
            op->enabled = strcmp(buf, "en") == 0;
        } else if (strcmp(tc, "op") == 0) {
            hes_strlcpy(op->operation, sizeof(op->operation), buf);
        }
    }
    hes_query_elem_list_free(data);

    // the operand / result groups
    load_group(otNode, "inputs", parse_inputs, op);
    load_group(otNode, "inputParameters", parse_input_parameters, op);
    load_group(otNode, "outputParameters", parse_output_parameters, op);
    load_group(otNode, "outputs", parse_outputs, op);
}

/// Declares bm_evaluate_all ahead of its definition: evaluate_op() caches a
/// result and then calls it again, and it calls evaluate_op(), so one of the two
/// has to be declared before the other is defined.
static void bm_evaluate_all(binding_map_t* bm, hes_bus_t* bus);

////////////////////////////////////////////////////////////////////////////////
/// Loads every table with the given element name, wherever it sits in the
/// document. The binding map nests its <operationTable>/<addressingTable> under
/// <objectType transCode="bm">, but at a depth that is the document's business,
/// so this is a descendant search rather than a fixed path.
static void bm_load_tables(hes_query_ctx_t* ctx,
                           const char* name,
                           void (*loader)(hes_query_elem_t*, binding_map_t*),
                           binding_map_t* bm)
{
    size_t n = 0;
    hes_query_elem_t** tables = hes_query_find_all(ctx, name, &n);
    for (size_t i = 0; i < n; i++) {
        loader(tables[i], bm);
    }

    hes_query_elem_list_free(tables);
}

////////////////////////////////////////////////////////////////////////////////
/// Performs one built-in operation on two operands (18012-3 Table 26):
/// 'gt' (>), 'lt' (<), 'eq' (==), 'ad' (+), 'su' (-), 'mu' (*), 'dv' (/, 0 when
/// the divisor is 0) and 'no' (NOT: 1 when the operand is 0).
///
/// 'ap' (appService) is deliberately not here: that operation is performed by
/// the actor's own app, so evaluate_op() sends it to the host hook instead. An
/// unrecognized code is reported on stderr and treated as 0.
///
/// @param opcode The operation code, from the row's 'op' datum.
/// @param a      The first operand.
/// @param b      The second operand, or a preset literal; 0 when the row has
///               only one operand.
/// @return The operation's result, or 0.0 for an unsupported code.
static double apply_operation(const char* opcode, double a, double b)
{
    if (strcmp(opcode, "gt") == 0) {
        return (a > b) ? 1.0 : 0.0;
    }

    if (strcmp(opcode, "lt") == 0) {
        return (a < b) ? 1.0 : 0.0;
    }

    if (strcmp(opcode, "eq") == 0) {
        return (a == b) ? 1.0 : 0.0;
    }

    if (strcmp(opcode, "ad") == 0) {
        return a + b;
    }

    if (strcmp(opcode, "su") == 0) {
        return a - b;
    }

    if (strcmp(opcode, "mu") == 0) {
        return a * b;
    }

    if (strcmp(opcode, "dv") == 0) {
        return (b != 0.0) ? a / b : 0.0;
    }

    if (strcmp(opcode, "no") == 0) {
        return (a != 0.0) ? 0.0 : 1.0;
    }

    // NOTE: 'ap' (appService -- 18012-3 Table 26 / Table 124, "operations
    // performed by the application service") is deliberately NOT handled
    // here: evaluate_op() dispatches it to the host's app_operation hook.
    // Absolute value is the conditioning/adjustment code 'av'
    // (apply_condition() below), not an operation code.
    fprintf(stderr, "binding_map[processor]: unsupported operation code '%s'\n", opcode);
    return 0.0;
}

////////////////////////////////////////////////////////////////////////////////
/// Is this row ready to be evaluated? It is ready when every input it has is
/// both usable and known:
///
///   - the row has at least one input (a row without inputs could not decide
///     anything);
///   - no input is blocked -- input authorType 'bk' means "this input may not
///     drive the row at all" (18012-3 Table 134) -- unless authorization has
///     been disabled (see bm_disable_authorization());
///   - every input's deviceIndex is in range and has a value (value_valid[]),
///     i.e. that source has reported at least once since startup.
///
/// @param bm The binding map holding the value cache.
/// @param op The row to test.
/// @return 1 when the row may be evaluated, 0 when it must be left alone.
static int op_ready(const binding_map_t* bm, const bm_operation_t* op)
{
    if (op->n_inputs == 0) {
        return 0;
    }

    for (int i = 0; i < op->n_inputs; i++) {
        // Input authorType 'bk' (18012-3 Table 134): this input may not drive
        // the row at all, so the row never fires. Empty means 'fl'.
        if (!bm->authz_disabled && strcmp(op->inputs[i].author_type, "bk") == 0) {
            return 0;
        }

        uint32_t di = op->inputs[i].device_index;
        if (di == 0 || di > BM_MAX_ADDRS || !bm->value_valid[di]) {
            return 0;
        }
    }

    return 1;
}

////////////////////////////////////////////////////////////////////////////////
/// Applies a 'co'/'ad'-style conditioning code (pt/bk/av/ng) to *v.
///
/// Used both on a row's inputs (the 'co' datum, 18012-3 Table 134) and on the
/// freshly computed result (the output 'ad' adjustment, Table 135):
///
///   - "" / "pt"  pass-through: leave *v as it is;
///   - "bk"       block: the caller must not fire the operation at all;
///   - "av"       absolute value: *v = |*v|;
///   - "ng"       negate: *v = -*v.
///
/// An unrecognized code is logged and treated as pass-through.
///
/// @param code The conditioning code (never NULL; empty behaves like "pt").
/// @param v    The operand or result to condition in place.
/// @return 1 when the operation must be blocked, 0 to continue with the
///         conditioned value.
static int apply_condition(const char* code, double* v)
{
    if (!code[0] || strcmp(code, "pt") == 0) {
        return 0;
    }

    if (strcmp(code, "bk") == 0) {
        return 1;
    }

    if (strcmp(code, "av") == 0) {
        if (*v < 0) {
            *v = -*v;
        }
        return 0;
    }

    if (strcmp(code, "ng") == 0) {
        *v = -(*v);
        return 0;
    }

    log_error(
            "unsupported conditioning code '%s' (treated as "
            "pass-through)\n",
            code);

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Does any input of this row require per-message authorization?
/// 'ap' (accompanyOperation) = 'au' means "authorize the accompanied
/// information" -- 18012-3 Table 134.
static int op_requires_authorization(const bm_operation_t* op)
{
    for (int i = 0; i < op->n_inputs; i++) {
        if (strcmp(op->inputs[i].accompany_op, "au") == 0) {
            return 1;
        }
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// May this row's output go out? Both output-side authorization gates live
/// here, so that "authorization is off" has exactly one place to be honoured:
///
///   - output authorType (18012-3 Table 135): 'bk' blocks the message. 'pa'
///     (partial) has no precise definition in the standard, so it is also
///     treated as blocked (fail closed) until its semantics are pinned down;
///   - Mode B -- per-message authorization. An input marked ap='au' requires
///     the accompanied information on the incoming message (ci/un) to be
///     authorized. Unlike 'at' above, this CAN vary per sender, which is why
///     it is decided here and not pre-computed into the row. Fail closed:
///     with no hook or no current message, the output is suppressed.
///
/// @param bm The binding map holding the row.
/// @param op The row that is about to output.
/// @return 1 when the output may proceed, 0 when it must be suppressed.
static int output_authorized(const binding_map_t* bm, const bm_operation_t* op)
{
    if (bm->authz_disabled) {
        return 1;
    }

    if (!strcmp(op->out_author_type, "bk") || !strcmp(op->out_author_type, "pa")) {
        log_error("PUT %s suppressed (authorType='%s')", op->out_dest_object, op->out_author_type);
        return 0;
    }

    if (op_requires_authorization(op)) {
        char at_addr[HES_PATH_MAX] = {0};
        bm_at_address(op, at_addr, sizeof(at_addr));
        if (!bm->auth_authorize || !bm->current_msg ||
            !bm->auth_authorize(bm->auth_ctx, bm->current_msg, at_addr)) {
            log_error("PUT %s suppressed (authorization denied)", op->out_dest_object);
            return 0;
        }
    }

    return 1;
}

////////////////////////////////////////////////////////////////////////////////
/// Evaluates one operation row: it takes its inputs, computes the result, and --
/// when the result is both new and allowed out -- sends the PUT it describes.
///
/// The steps, and why they are in this order:
///
///   1. op_ready() decides whether the row can run at all: every input must be
///      known, i.e. its source has reported at least once.
///   2. Every input is conditioned ('co') and collected as an operand.
///   3. Change detection over those operands: the same operands as last time end
///      the call here, so an unchanged situation neither recomputes the row nor
///      re-enters a stateful app.
///   4. The operation itself runs: the built-in set through apply_operation(),
///      or, for 'ap' (appService), the host's app_operation hook.
///   5. The output gates run BEFORE anything is committed -- change detection,
///      then 'at', then per-message (Mode B) authorization. A suppressed output
///      must leave no trace in the value cache or in the change detector:
///      otherwise an unauthorized sender could reach the target indirectly
///      through a downstream chained row, or neutralize a later authorized
///      command by going first with the same value.
///   6. Only now is the result recorded and sent: cached at out_device_index
///      (which may unlock downstream 'it'-chained rows, hence the recursion into
///      bm_evaluate_all()), then published as a PUT on the bus.
///
/// @param bm  The binding map holding the row and the value cache.
/// @param bus The bus to send the resulting PUT on.
/// @param op  The row to evaluate.
static void evaluate_op(binding_map_t* bm, hes_bus_t* bus, bm_operation_t* op)
{
    if (!op->enabled || !op_ready(bm, op)) {
        return;
    }

    // Condition every input of the row into the operand list. A 'bk' on any
    // input blocks the whole row (as before); the rest of the operands are
    // handed to the app when the operation is 'ap'.
    bm_operand_t operands[BM_MAX_INPUTS];
    int n_operands = 0;
    for (int i = 0; i < op->n_inputs; i++) {
        double v = bm->value_cache[op->inputs[i].device_index];
        if (apply_condition(op->inputs[i].condition, &v)) {
            return;  // 'bk' on this input: don't fire
        }

        operands[n_operands].device_index = op->inputs[i].device_index;
        hes_strlcpy(operands[n_operands].source_object, HES_PATH_MAX, op->inputs[i].source_object);
        operands[n_operands].value = v;
        n_operands++;
    }
    if (n_operands == 0) {
        return;
    }

    // The built-in operators use at most two operands: the first input, and
    // either the second input or a preset literal from inputParameters.
    double a = operands[0].value;
    double b = 0.0;
    if (n_operands >= 2) {
        b = operands[1].value;
    } else if (op->n_input_params >= 2 && !op->ip_is_live[1]) {
        b = op->input_params[1];
    }

    // Change-detection over every operand: re-evaluation with the very same
    // inputs must not recompute the row or (for an appService) call the app
    // again -- an app may be stateful.
    int inputs_changed = !op->has_last_inputs || op->n_last_operands != n_operands;
    for (int i = 0; !inputs_changed && i < n_operands; i++) {
        if (op->last_operands[i] != operands[i].value) {
            inputs_changed = 1;
        }
    }
    op->has_last_inputs = 1;
    op->n_last_operands = n_operands;
    for (int i = 0; i < n_operands; i++) {
        op->last_operands[i] = operands[i].value;
    }
    if (!inputs_changed) {
        return;  // same operands as last time -- nothing new to compute or send
    }

    double result;
    if (!strcmp(op->operation, "ap")) {
        // appService: the customer-specific protected app performs this
        // operation, so hand it every operand and use its result.
        // Mirrors 18012-3 Table 26 / A.2.2 ("the appService performs its
        // operations in accordance with the application service").
        if (!bm->app_operation) {
            log_error(
                    "op ri=%u is appService ('ap') but no app bridge is "
                    "registered -- skipped",
                    op->ref_id);
            return;
        }
        if (bm->app_operation(bm->app_ctx, op, operands, n_operands, &result) != 0) {
            log_error("appService op ri=%u could not be performed by the app", op->ref_id);
            return;
        }
    } else {
        result = apply_operation(op->operation, a, b);
    }

    if (apply_condition(op->out_adjustment, &result)) {
        return;  // 'bk' on the output: suppress entirely
    }

    if (op->has_last_output && result == op->last_output_value) {
        return;  // no change -- don't re-fire
    }

    // (a, b) are the built-in operations' operands. An appService row has none --
    // the app computes the result -- so it prints just its operation and result
    // rather than two numbers that mean nothing for it.
    if (strcmp(op->operation, "ap") == 0) {
        log_info("op ri=%u (ap) = %.2f", op->ref_id, result);
    } else {
        log_info("op ri=%u (%s %.2f, %.2f) = %.2f", op->ref_id, op->operation, a, b, result);
    }

    // ---- the output gates, BEFORE any state is touched ----
    //
    // The order here is the whole point, and getting it wrong is subtle. A
    // suppressed output must leave NO trace: not the outgoing message, not the
    // value cache, not the chaining recursion, and not the change detector.
    //
    // Deciding after the fact instead would mean two things:
    //
    //  1. The value cache would hold a value the destination never received. A
    //     downstream row chained off this one reads its inputs out of that same
    //     cache, so it would act on a value that authorization had just refused
    //     -- reaching the target indirectly, which is no refusal at all.
    //  2. The row would count as having already produced its value, so the next
    //     AUTHORIZED command carrying the same value would be swallowed as "no
    //     change". An unauthorized sender could neutralize a legitimate one
    //     simply by going first.
    //
    // So: authorize first, commit second. The gates themselves are in
    // output_authorized(), which is also where --no-authz is honoured.
    if (op->out_dest_object[0] != '\0' && !output_authorized(bm, op)) {
        return;
    }

    // ---- the output is going to happen, so now record it ----
    op->has_last_output = 1;
    op->last_output_value = result;

    if (op->out_device_index > 0 && op->out_device_index <= BM_MAX_ADDRS) {
        bm->value_cache[op->out_device_index] = result;
        bm->value_valid[op->out_device_index] = 1;
        // This may unlock a downstream operation chained off this
        // internal ('it') value (e.g. ot3 depends on ot1's and ot2's
        // outputs) -- re-evaluate the table. Bounded by the has_last_output
        // change-detection above, so this terminates for the acyclic
        // (DAG-shaped) operation tables the standard describes.
        bm_evaluate_all(bm, bus);
    }

    if (op->out_dest_object[0] != '\0') {
        hes_clme_msg_t msg = {0};
        msg.verb = HES_VERB_PUT;
        msg.device_index = op->out_device_index;
        hes_msg_set_path(&msg, op->out_dest_object);

        char buf[32] = {0};
        snprintf(buf, sizeof(buf), "%g", result);
        hes_msg_set_payload_str(&msg, buf);
        hes_bus_send(bus, &msg);

        // log_info, like the row trace above: "row evaluated -> PUT sent" is the
        // pair worth reading together, so -v shows the whole story.
        log_info("PUT %s = %s (deviceList=%u)", op->out_dest_object, buf, op->out_device_index);
    }
}

////////////////////////////////////////////////////////////////////////////////
/// Evaluates every row of the binding map, in table order.
///
/// Called when a report has been taken into the value cache, and from
/// evaluate_op() itself after a result is cached -- a fresh internal ('it') value
/// may be the last missing input of a downstream row.
///
/// The recursion terminates because a row whose operands did not change stops at
/// its own change detection, so nothing new can propagate.
///
/// @param bm  The binding map to evaluate.
/// @param bus The bus to send any resulting PUT messages on.
static void bm_evaluate_all(binding_map_t* bm, hes_bus_t* bus)
{
    for (int i = 0; i < bm->n_ops; i++) {
        evaluate_op(bm, bus, &bm->ops[i]);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

void bm_init(binding_map_t* bm)
{
    memset(bm, 0, sizeof(*bm));
}

int bm_load_xml(binding_map_t* bm, const char* path)
{
    hes_query_ctx_t* ctx = hes_query_open(path);
    if (ctx == NULL) {
        log_error("failed to parse %s", path);
        return -1;
    }

    bm_load_tables(ctx, "operationTable", parse_operation_table, bm);
    bm_load_tables(ctx, "addressingTable", parse_addressing_table, bm);

    hes_query_close(ctx);

    log_info("loaded %d operation row(s), %d addressing row(s) from %s", bm->n_ops, bm->n_addrs,
             path);
    return 0;
}

const bm_addr_t* bm_find_addr(const binding_map_t* bm, uint32_t device_index)
{
    for (int i = 0; i < bm->n_addrs; i++) {
        if (bm->addrs[i].device_index == device_index) {
            return &bm->addrs[i];
        }
    }

    return NULL;
}

void bm_at_address(const bm_operation_t* op, char* buf, size_t buf_size)
{
    snprintf(buf, buf_size, "/lx/ob/bm/%s/%s/at", op->table_code, op->out_code);
}

int bm_set_at(binding_map_t* bm, const char* at_address, const char* value)
{
    for (int i = 0; i < bm->n_ops; i++) {
        char addr[HES_PATH_MAX] = {0};
        bm_at_address(&bm->ops[i], addr, sizeof(addr));
        if (strcmp(addr, at_address) != 0) {
            continue;
        }

        hes_strlcpy(bm->ops[i].out_author_type, sizeof(bm->ops[i].out_author_type), value);
        return 0;
    }

    return -1;
}

void bm_disable_authorization(binding_map_t* bm)
{
    bm->authz_disabled = 1;
}

int bm_is_declared_destination(const binding_map_t* bm, const char* path, uint32_t device_index)
{
    if (!path || path[0] == '\0' || device_index == 0) {
        return 0;
    }

    const bm_addr_t* addr = bm_find_addr(bm, device_index);
    if (!addr) {
        return 0;
    }

    // Only a real device behind a HAN/WAN interface module is a valid
    // command destination -- see bm.h.
    return addr->module_type == HES_MODTYPE_HI || addr->module_type == HES_MODTYPE_WI;
}

void bm_controller_start(binding_map_t* bm, hes_bus_t* bus)
{
    // A record of the (path, deviceIndex) pairs we have already subscribed to.
    // The same source object often feeds several operation rows, so one
    // subscription per (object, device) is enough. One subscription per path is
    // not enough: two devices may expose the same object path, and each one
    // needs its own subscription.
    typedef struct bm_sub_key {
        char path[HES_PATH_MAX];
        uint32_t device_index;
    } bm_sub_key_t;
    bm_sub_key_t sent[BM_MAX_ADDRS];
    int n_sent = 0;

    for (int i = 0; i < bm->n_ops; i++) {
        bm_operation_t* op = &bm->ops[i];
        if (!op->enabled) {
            // Skip operation groups whose 'en' (enable) flag is not set. A
            // disabled rule must not cause a subscription.
            continue;
        }

        for (int j = 0; j < op->n_inputs; j++) {
            bm_input_t* in = &op->inputs[j];

            // An input with no source path is an internal chain: its value comes
            // from another operation's output inside this same binding map (the
            // 'it' internal-process values). There is no external module to
            // subscribe to, so there is nothing to do here. The processor fills
            // those values when the upstream operation fires.
            if (in->source_object[0] == '\0') {
                continue;
            }

            // If we already subscribed to this (path, device) pair (from an
            // earlier row), skip.
            int already = 0;
            for (int k = 0; k < n_sent; k++) {
                if (sent[k].device_index == in->device_index &&
                    strcmp(sent[k].path, in->source_object) == 0) {
                    already = 1;
                    break;
                }
            }
            if (already) {
                continue;
            }

            // Look up which module owns that device in the addressingTable. A
            // subscription is sent only when the owner is an external module
            // (hi HAN, wi WAN, or sm service module). If the device is not in
            // the table, or it is an internal process (it) or unknown, there is
            // no remote publisher to talk to, so skip it.
            const bm_addr_t* addr = bm_find_addr(bm, in->device_index);
            if (!addr || addr->module_type == HES_MODTYPE_IT ||
                addr->module_type == HES_MODTYPE_NONE) {
                continue;
            }

            // This is a HES-CLME subscribe sent out on the event bus. In the
            // poc2 hub-and-leaf topology it goes to the interface modules, so
            // they start delivering event reports for that object back to the
            // core module.
            hes_clme_msg_t msg = {0};
            msg.verb = HES_VERB_SUBSCRIBE;              // primitive action "subscribe" (18012-3)
            msg.device_index = in->device_index;        // which device's object we care about
            hes_msg_set_path(&msg, in->source_object);  // the Lexicon path, e.g. /lx/ob/...
            hes_bus_send(bus, &msg);

            fprintf(stderr, "binding_map[controller]: SUBSCRIBE %s (deviceIndex=%u)\n",
                    in->source_object, in->device_index);

            // Record the (path, device) pair so it isn't subscribed twice.
            if (n_sent < BM_MAX_ADDRS) {
                hes_strlcpy(sent[n_sent].path, HES_PATH_MAX, in->source_object);
                sent[n_sent].device_index = in->device_index;
                n_sent++;
            }
        }
    }
}

double bm_datum_value(const char* payload, const char* trans_code)
{
    if (payload == NULL) {
        return 0.0;
    }

    size_t want = (trans_code != NULL) ? strlen(trans_code) : 0;
    int structured = 0;

    const char* p = payload;
    while (*p != '\0') {
        const char* end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        const char* eq = memchr(p, '=', len);
        if (eq != NULL) {
            structured = 1;
            size_t name_len = (size_t)(eq - p);
            if (want > 0 && name_len == want && strncmp(p, trans_code, want) == 0) {
                return atof(eq + 1);
            }
        }

        if (end == NULL) {
            break;
        }
        p = end + 1;
    }

    // A table answer that simply does not carry that datum: report no value
    // rather than hand back some other datum's number.
    if (structured) {
        return 0.0;
    }

    // Not a table at all: the payload IS the value (an interface module's
    // report, or the pre-lexicon bare numbers this POC started with).
    return atof(payload);
}

void bm_processor_handle(binding_map_t* bm, hes_bus_t* bus, const hes_clme_msg_t* in)
{
    // Only a message that REPORTS a value is accepted, because a reported value
    // is the only thing this function can use: it stores it in the cache for the
    // rules to read. Two verbs report a value:
    //
    //   - EVENT: the source pushes a value by itself -- the normal input, e.g.
    //     an interface module reporting a sensor reading or a device state;
    //   - GET: the source sends the value as the answer to a read.
    //
    // Everything else is refused:
    //
    //   - PUT is a COMMAND to a device, not a report from it: it asks the device
    //     to become something, and says nothing about what the device reported.
    //     Storing it would make a rule act on a reading nobody took, and would
    //     then mask the device's real report of that same number -- the failure
    //     the payload check below is about;
    //   - SUBSCRIBE / UNSUBSCRIBE are bus plumbing and carry no value;
    //   - NONE (0) means the verb was never set: an empty or malformed message.
    //
    // Every answerer in this tree publishes its answer as an EVENT, not as a GET
    // (see src/common/hes_dispatch.c, src/han/wifi/wifi.c), so a GET that
    // arrives here is in practice always a *request* -- which the payload check
    // below refuses. GET stays accepted because the verb itself means "read, and
    // the answer carries a value".
    if (in->verb != HES_VERB_EVENT && in->verb != HES_VERB_GET) {
        return;
    }

    // Only a message that carries a value may update the cache, so a message
    // with an empty payload is ignored.
    //
    // Two kinds of message arrive with no payload, and neither is a report:
    //
    //   - a GET *request* -- a client has only a path, it is asking a question;
    //   - an EVENT that carries no value.
    //
    // Both would be read as the number 0, because bm_datum_value("") is
    // atof("") = 0.0, and that 0 would be stored as "what this source
    // reported". Two things go wrong then:
    //
    //   1. a rule reading that input acts on a value nobody measured;
    //   2. the real report that follows is compared against that 0, so a report
    //      whose value is 0 counts as "nothing new" and is dropped.
    //
    // Example: a client sends a GET for the button object, just to read it, so
    // the message has no payload. Without this guard the button's cached value
    // becomes 0, the row's "button > 0" test fails, and the light is switched
    // OFF -- because someone asked a question, not because anyone pressed the
    // button.
    //
    // No real answer is lost this way: an interface module answers a GET by
    // publishing an EVENT with the value in it (see src/han/wifi/wifi.c).
    if (in->payload[0] == '\0') {
        return;
    }

    // Mode B authorization is decided per message, and this is the message.
    bm->current_msg = in;

    // The value of the currentValue datum ('va'), which is what an operation
    // table's live inputParameter refers to.
    double v = bm_datum_value(in->payload, BM_LIVE_INPUT_PARAM);
    int updated_any = 0;

    for (int i = 0; i < bm->n_ops; i++) {
        bm_operation_t* op = &bm->ops[i];
        for (int j = 0; j < op->n_inputs; j++) {
            // Attribute by (path, deviceIndex), not path alone: when two
            // devices share one object path (two identical bulbs behind one
            // module), the event's device_index selects WHICH input row this
            // value belongs to. device_index == 0 is accepted as a legacy
            // fallback for senders that don't stamp it (single-device POC).
            if (op->inputs[j].source_object[0] &&
                strcmp(op->inputs[j].source_object, in->path) == 0 &&
                (in->device_index == 0 || op->inputs[j].device_index == in->device_index)) {
                uint32_t di = op->inputs[j].device_index;
                if (di > 0 && di <= BM_MAX_ADDRS) {
                    bm->value_cache[di] = v;
                    bm->value_valid[di] = 1;
                    updated_any = 1;
                }
            }
        }
    }

    if (updated_any) {
        bm_evaluate_all(bm, bus);
    }
}
