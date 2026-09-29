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
/// @file
/// @brief Implementation of the native struct mapping (hes_gateway.h): one
/// loader per element type, and the matching _free() for each.
///
/// @details
/// The shape of the file is a loader/free pair per element type, all built on two
/// things:
///
///   - _str(): a strdup() that never returns NULL, so a missing element becomes
///     "" instead of a NULL dereference later.
///
///   - HES_GW_LOAD_LIST(): expands to the Approach A list pattern -- allocate
///     count + 1 entries, fill them by re-querying each indexed path, then store
///     a trailing NULL sentinel. An interior NULL from a failed loader stops the
///     fill rather than being stored.
///
/// Paths are built in fixed 600-byte buffers. -Wformat-truncation is disabled
/// around this file (see the comment at the pragma): at -O3 GCC cannot see that
/// these paths are short, and enlarging the buffer does not help because its
/// estimate scales with the buffer. The real depth is bounded by the lexicon
/// schema.
///
/// Memory/ownership: everything is allocated here and owned by the caller's tree;
/// each loader has a matching _free(). Strings are strdup()'d.
///
/// Threading: no shared state; a tree is built and freed by one thread.
#include "hes_gateway.h"

#include "hes_query.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

////////////////////////////////////////////////////////////////////////////////
// Small helpers
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// strdup() that never returns NULL: missing values become "".
static char* _str(const char* s)
{
    return (s != NULL) ? strdup(s) : strdup("");
}

////////////////////////////////////////////////////////////////////////////////
// Wformat-truncation (level 1, enabled by -Wall) cannot see that the paths
////////////////////////////////////////////////////////////////////////////////
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

/// Load a list of <elem_name> children of `base` using Approach A
/// (pointer-to-pointer array).  `loader` fills each element from its own path.
#define HES_GW_LOAD_LIST(ctx, base, elem_name, arr, count, loader)               \
    do {                                                                         \
        char _p[600];                                                            \
        snprintf(_p, sizeof(_p), "%s.%s", (base), (elem_name));                  \
        size_t _n = 0;                                                           \
        hes_query_elem_t** _es = hes_query_elem_list((ctx), _p, &_n);            \
        (count) = 0;                                                             \
        (arr) = NULL;                                                            \
        if (_n > 0) {                                                            \
            (arr) = calloc(_n + 1, sizeof(*(arr)));                              \
            for (size_t _i = 0; _i < _n; _i++) {                                 \
                snprintf(_p, sizeof(_p), "%s.%s[%zu]", (base), (elem_name), _i); \
                void* _item = (loader)((ctx), _p);                               \
                if (_item == NULL)                                               \
                    break; /* never store interior NULLs */                      \
                (arr)[(count)++] = _item;                                        \
            }                                                                    \
            (arr)[(count)] = NULL; /* NULL terminator sentinel */                \
        }                                                                        \
        hes_query_elem_list_free(_es); /* no-op when nothing was loaded */       \
    } while (0)

////////////////////////////////////////////////////////////////////////////////
/// Load the common attributes shared by container elements.
static void _load_common_attrs(hes_query_ctx_t* ctx,
                               const char* base,
                               char** version,
                               char** addr,
                               char** trans,
                               char** desc,
                               char** purpose)
{
    char p[600];
    snprintf(p, sizeof(p), "%s.@versionNumber", base);
    *version = _str(hes_query_string(ctx, p));
    snprintf(p, sizeof(p), "%s.@addrPoint", base);
    *addr = _str(hes_query_string(ctx, p));
    snprintf(p, sizeof(p), "%s.@transCode", base);
    *trans = _str(hes_query_string(ctx, p));
    snprintf(p, sizeof(p), "%s.@descriptiveName", base);
    *desc = _str(hes_query_string(ctx, p));
    if (purpose != NULL) {
        snprintf(p, sizeof(p), "%s.@dataPurpose", base);
        *purpose = _str(hes_query_string(ctx, p));
    }
}

////////////////////////////////////////////////////////////////////////////////
// Leaf / element loaders
////////////////////////////////////////////////////////////////////////////////

static hes_gw_data_t* _load_data(hes_query_ctx_t* ctx, const char* base)
{
    hes_gw_data_t* d = (hes_gw_data_t*)calloc(1, sizeof(*d));
    if (d == NULL) {
        return NULL;
    }

    char p[600];
    snprintf(p, sizeof(p), "%s.@versionNumber", base);
    d->version_number = _str(hes_query_string(ctx, p));
    snprintf(p, sizeof(p), "%s.@addrPoint", base);
    d->addr_point = _str(hes_query_string(ctx, p));
    snprintf(p, sizeof(p), "%s.@transCode", base);
    d->trans_code = _str(hes_query_string(ctx, p));
    snprintf(p, sizeof(p), "%s.@descriptiveName", base);
    d->descriptive_name = _str(hes_query_string(ctx, p));
    snprintf(p, sizeof(p), "%s.@dataPurpose", base);
    d->data_purpose = _str(hes_query_string(ctx, p));
    snprintf(p, sizeof(p), "%s.@dataFormat", base);
    d->data_format = _str(hes_query_string(ctx, p));
    snprintf(p, sizeof(p), "%s.@memoryType", base);
    d->memory_type = _str(hes_query_string(ctx, p));
    snprintf(p, sizeof(p), "%s.@storageType", base);
    d->storage_type = _str(hes_query_string(ctx, p));
    snprintf(p, sizeof(p), "%s.@enumerated", base);
    d->enumerated = _str(hes_query_string(ctx, p));
    snprintf(p, sizeof(p), "%s.@description", base);
    d->description = _str(hes_query_string(ctx, p));
    snprintf(p, sizeof(p), "%s.@preassigned", base);
    d->preassigned = _str(hes_query_string(ctx, p));
    // text content
    snprintf(p, sizeof(p), "%s", base);
    d->value = _str(hes_query_string(ctx, p));
    return d;
}

////////////////////////////////////////////////////////////////////////////////
/// Loads one <inputs> group: the five attributes every container shares, then
/// the group's <data> leaves.
///
/// @param ctx  The query context holding the document.
/// @param base Path of the element within that document.
/// @return The new group, or NULL when it could not be allocated.
static hes_gw_inputs_t* _load_inputs(hes_query_ctx_t* ctx, const char* base)
{
    hes_gw_inputs_t* in = (hes_gw_inputs_t*)calloc(1, sizeof(*in));
    if (in == NULL) {
        return NULL;
    }
    _load_common_attrs(ctx, base, &in->version_number, &in->addr_point, &in->trans_code,
                       &in->descriptive_name, &in->data_purpose);
    HES_GW_LOAD_LIST(ctx, base, "data", in->data, in->data_count, _load_data);
    return in;
}

////////////////////////////////////////////////////////////////////////////////
/// Loads one <outputs> group: the five shared attributes, then the group's
/// <data> leaves.
///
/// @param ctx  The query context holding the document.
/// @param base Path of the element within that document.
/// @return The new group, or NULL when it could not be allocated.
static hes_gw_outputs_t* _load_outputs(hes_query_ctx_t* ctx, const char* base)
{
    hes_gw_outputs_t* out = (hes_gw_outputs_t*)calloc(1, sizeof(*out));
    if (out == NULL) {
        return NULL;
    }
    _load_common_attrs(ctx, base, &out->version_number, &out->addr_point, &out->trans_code,
                       &out->descriptive_name, &out->data_purpose);
    HES_GW_LOAD_LIST(ctx, base, "data", out->data, out->data_count, _load_data);
    return out;
}

////////////////////////////////////////////////////////////////////////////////
/// Loads one <inputParameters> or <outputParameters> group: the five shared
/// attributes, its own @description, then its <data> leaves. Both kinds of group
/// have the same shape, so one loader serves them.
///
/// @param ctx  The query context holding the document.
/// @param base Path of the element within that document.
/// @return The new group, or NULL when it could not be allocated.
static hes_gw_param_group_t* _load_param_group(hes_query_ctx_t* ctx, const char* base)
{
    hes_gw_param_group_t* g = (hes_gw_param_group_t*)calloc(1, sizeof(*g));
    if (g == NULL) {
        return NULL;
    }
    _load_common_attrs(ctx, base, &g->version_number, &g->addr_point, &g->trans_code,
                       &g->descriptive_name, &g->data_purpose);
    char p[600];
    snprintf(p, sizeof(p), "%s.@description", base);
    g->description = _str(hes_query_string(ctx, p));
    HES_GW_LOAD_LIST(ctx, base, "data", g->data, g->data_count, _load_data);
    return g;
}

////////////////////////////////////////////////////////////////////////////////
/// Loads one <operationTable>: the five shared attributes, its own <data>
/// leaves, and its <inputs>, <inputParameters>, <outputParameters> and <outputs>
/// groups (a table may hold more than one of each).
///
/// @param ctx  The query context holding the document.
/// @param base Path of the element within that document.
/// @return The new table, or NULL when it could not be allocated.
static hes_gw_operation_table_t* _load_operation_table(hes_query_ctx_t* ctx, const char* base)
{
    hes_gw_operation_table_t* ot = (hes_gw_operation_table_t*)calloc(1, sizeof(*ot));
    if (ot == NULL) {
        return NULL;
    }
    _load_common_attrs(ctx, base, &ot->version_number, &ot->addr_point, &ot->trans_code,
                       &ot->descriptive_name, &ot->data_purpose);

    HES_GW_LOAD_LIST(ctx, base, "data", ot->data, ot->data_count, _load_data);
    HES_GW_LOAD_LIST(ctx, base, "inputs", ot->inputs, ot->inputs_count, _load_inputs);
    HES_GW_LOAD_LIST(ctx, base, "inputParameters", ot->input_parameters, ot->input_parameters_count,
                     _load_param_group);
    HES_GW_LOAD_LIST(ctx, base, "outputParameters", ot->output_parameters,
                     ot->output_parameters_count, _load_param_group);
    HES_GW_LOAD_LIST(ctx, base, "outputs", ot->outputs, ot->outputs_count, _load_outputs);
    return ot;
}

////////////////////////////////////////////////////////////////////////////////
/// Loads one <addressingTable>: the five shared attributes and its <data>
/// leaves (deviceIndex, moduleType, moduleRefIndex, netRefIndex and address).
///
/// @param ctx  The query context holding the document.
/// @param base Path of the element within that document.
/// @return The new row, or NULL when it could not be allocated.
static hes_gw_addressing_table_t* _load_addressing_table(hes_query_ctx_t* ctx, const char* base)
{
    hes_gw_addressing_table_t* at = (hes_gw_addressing_table_t*)calloc(1, sizeof(*at));
    if (at == NULL) {
        return NULL;
    }
    _load_common_attrs(ctx, base, &at->version_number, &at->addr_point, &at->trans_code,
                       &at->descriptive_name, &at->data_purpose);
    HES_GW_LOAD_LIST(ctx, base, "data", at->data, at->data_count, _load_data);
    return at;
}

////////////////////////////////////////////////////////////////////////////////
/// Loads one <objectType>: the four attributes it has, its <data> leaves, and
/// its <operationTable> and <addressingTable> children.
///
/// This element carries no dataPurpose, which is why NULL is passed for it.
///
/// @param ctx  The query context holding the document.
/// @param base Path of the element within that document.
/// @return The new object type, or NULL when it could not be allocated.
static hes_gw_object_type_t* _load_object_type(hes_query_ctx_t* ctx, const char* base)
{
    hes_gw_object_type_t* ot = (hes_gw_object_type_t*)calloc(1, sizeof(*ot));
    if (ot == NULL) {
        return NULL;
    }
    _load_common_attrs(ctx, base, &ot->version_number, &ot->addr_point, &ot->trans_code,
                       &ot->descriptive_name, NULL);

    HES_GW_LOAD_LIST(ctx, base, "data", ot->data, ot->data_count, _load_data);
    HES_GW_LOAD_LIST(ctx, base, "operationTable", ot->operation_tables, ot->operation_table_count,
                     _load_operation_table);
    HES_GW_LOAD_LIST(ctx, base, "addressingTable", ot->addressing_tables,
                     ot->addressing_table_count, _load_addressing_table);
    return ot;
}

////////////////////////////////////////////////////////////////////////////////
/// Loads one <lexiconType>: the four attributes it has (no dataPurpose) and its
/// <objectType> children.
///
/// @param ctx  The query context holding the document.
/// @param base Path of the element within that document.
/// @return The new lexicon type, or NULL when it could not be allocated.
static hes_gw_lexicon_type_t* _load_lexicon_type(hes_query_ctx_t* ctx, const char* base)
{
    hes_gw_lexicon_type_t* lx = (hes_gw_lexicon_type_t*)calloc(1, sizeof(*lx));
    if (lx == NULL) {
        return NULL;
    }
    _load_common_attrs(ctx, base, &lx->version_number, &lx->addr_point, &lx->trans_code,
                       &lx->descriptive_name, NULL);
    HES_GW_LOAD_LIST(ctx, base, "objectType", lx->object_types, lx->object_type_count,
                     _load_object_type);
    return lx;
}

////////////////////////////////////////////////////////////////////////////////
/// Loads the single <overallProcess>: the four attributes it has (no
/// dataPurpose) and its <lexiconType> children, which are the root of the
/// process tree.
///
/// @param ctx  The query context holding the document.
/// @param base Path of the element within that document.
/// @return The new process, or NULL when it could not be allocated.
static hes_gw_overall_process_t* _load_overall_process(hes_query_ctx_t* ctx, const char* base)
{
    hes_gw_overall_process_t* op = (hes_gw_overall_process_t*)calloc(1, sizeof(*op));
    if (op == NULL) {
        return NULL;
    }
    _load_common_attrs(ctx, base, &op->version_number, &op->addr_point, &op->trans_code,
                       &op->descriptive_name, NULL);
    HES_GW_LOAD_LIST(ctx, base, "lexiconType", op->lexicon_types, op->lexicon_type_count,
                     _load_lexicon_type);
    return op;
}

////////////////////////////////////////////////////////////////////////////////
// Deep free (mirrors the loaders)
////////////////////////////////////////////////////////////////////////////////

static void hes_gw_data_free(hes_gw_data_t* d)
{
    if (d == NULL) {
        return;
    }

    free(d->version_number);
    free(d->addr_point);
    free(d->trans_code);
    free(d->descriptive_name);
    free(d->data_purpose);
    free(d->data_format);
    free(d->memory_type);
    free(d->storage_type);
    free(d->enumerated);
    free(d->description);
    free(d->preassigned);
    free(d->value);
    free(d);
}

////////////////////////////////////////////////////////////////////////////////
/// Frees an <inputs> group: its <data> leaves, its string fields, then the group
/// itself. NULL is accepted, so a caller may free a pointer it never set.
///
/// @param in The group to free.
static void hes_gw_inputs_free(hes_gw_inputs_t* in)
{
    if (in == NULL) {
        return;
    }

    for (size_t i = 0; i < in->data_count; i++) {
        hes_gw_data_free(in->data[i]);
    }
    free(in->data);

    free(in->version_number);
    free(in->addr_point);
    free(in->trans_code);
    free(in->descriptive_name);
    free(in->data_purpose);
    free(in);
}

////////////////////////////////////////////////////////////////////////////////
/// Frees an <outputs> group: its <data> leaves, its string fields, then the
/// group itself. NULL is accepted.
///
/// @param out The group to free.
static void hes_gw_outputs_free(hes_gw_outputs_t* out)
{
    if (out == NULL) {
        return;
    }

    for (size_t i = 0; i < out->data_count; i++) {
        hes_gw_data_free(out->data[i]);
    }
    free(out->data);

    free(out->version_number);
    free(out->addr_point);
    free(out->trans_code);
    free(out->descriptive_name);
    free(out->data_purpose);
    free(out);
}

////////////////////////////////////////////////////////////////////////////////
/// Frees a parameter group: its <data> leaves, its string fields, then the group
/// itself. NULL is accepted.
///
/// @param g The group to free.
static void hes_gw_param_group_free(hes_gw_param_group_t* g)
{
    if (g == NULL) {
        return;
    }

    for (size_t i = 0; i < g->data_count; i++) {
        hes_gw_data_free(g->data[i]);
    }
    free(g->data);

    free(g->version_number);
    free(g->addr_point);
    free(g->trans_code);
    free(g->descriptive_name);
    free(g->data_purpose);
    free(g->description);
    free(g);
}

////////////////////////////////////////////////////////////////////////////////
/// Frees an <operationTable> and everything under it: its <data> leaves, its
/// inputs / inputParameters / outputParameters / outputs groups, its string
/// fields, then the table itself. NULL is accepted.
///
/// @param ot The table to free.
static void hes_gw_operation_table_free(hes_gw_operation_table_t* ot)
{
    if (ot == NULL) {
        return;
    }

    for (size_t i = 0; i < ot->data_count; i++) {
        hes_gw_data_free(ot->data[i]);
    }
    free(ot->data);

    for (size_t i = 0; i < ot->inputs_count; i++) {
        hes_gw_inputs_free(ot->inputs[i]);
    }
    free(ot->inputs);

    for (size_t i = 0; i < ot->input_parameters_count; i++) {
        hes_gw_param_group_free(ot->input_parameters[i]);
    }
    free(ot->input_parameters);

    for (size_t i = 0; i < ot->output_parameters_count; i++) {
        hes_gw_param_group_free(ot->output_parameters[i]);
    }
    free(ot->output_parameters);

    for (size_t i = 0; i < ot->outputs_count; i++) {
        hes_gw_outputs_free(ot->outputs[i]);
    }
    free(ot->outputs);

    free(ot->version_number);
    free(ot->addr_point);
    free(ot->trans_code);
    free(ot->descriptive_name);
    free(ot->data_purpose);
    free(ot);
}

////////////////////////////////////////////////////////////////////////////////
/// Frees an <addressingTable>: its <data> leaves, its string fields, then the
/// row itself. NULL is accepted.
///
/// @param at The row to free.
static void hes_gw_addressing_table_free(hes_gw_addressing_table_t* at)
{
    if (at == NULL) {
        return;
    }

    for (size_t i = 0; i < at->data_count; i++) {
        hes_gw_data_free(at->data[i]);
    }
    free(at->data);

    free(at->version_number);
    free(at->addr_point);
    free(at->trans_code);
    free(at->descriptive_name);
    free(at->data_purpose);
    free(at);
}

////////////////////////////////////////////////////////////////////////////////
/// Frees an <objectType> and everything under it: its <data> leaves, its
/// operation and addressing tables, its string fields, then the object type.
/// NULL is accepted.
///
/// @param ot The object type to free.
static void hes_gw_object_type_free(hes_gw_object_type_t* ot)
{
    if (ot == NULL) {
        return;
    }

    for (size_t i = 0; i < ot->data_count; i++) {
        hes_gw_data_free(ot->data[i]);
    }
    free(ot->data);

    for (size_t i = 0; i < ot->operation_table_count; i++) {
        hes_gw_operation_table_free(ot->operation_tables[i]);
    }
    free(ot->operation_tables);

    for (size_t i = 0; i < ot->addressing_table_count; i++) {
        hes_gw_addressing_table_free(ot->addressing_tables[i]);
    }
    free(ot->addressing_tables);

    free(ot->version_number);
    free(ot->addr_point);
    free(ot->trans_code);
    free(ot->descriptive_name);
    free(ot);
}

////////////////////////////////////////////////////////////////////////////////
/// Frees a <lexiconType> and everything under it: its object types, its string
/// fields, then the lexicon type. NULL is accepted.
///
/// @param lx The lexicon type to free.
static void hes_gw_lexicon_type_free(hes_gw_lexicon_type_t* lx)
{
    if (lx == NULL) {
        return;
    }

    for (size_t i = 0; i < lx->object_type_count; i++) {
        hes_gw_object_type_free(lx->object_types[i]);
    }
    free(lx->object_types);

    free(lx->version_number);
    free(lx->addr_point);
    free(lx->trans_code);
    free(lx->descriptive_name);
    free(lx);
}

////////////////////////////////////////////////////////////////////////////////
/// Frees the <overallProcess> and the whole process tree below it: its lexicon
/// types, its string fields, then the process. NULL is accepted.
///
/// @param op The process to free.
static void hes_gw_overall_process_free(hes_gw_overall_process_t* op)
{
    if (op == NULL) {
        return;
    }

    for (size_t i = 0; i < op->lexicon_type_count; i++) {
        hes_gw_lexicon_type_free(op->lexicon_types[i]);
    }
    free(op->lexicon_types);

    free(op->version_number);
    free(op->addr_point);
    free(op->trans_code);
    free(op->descriptive_name);
    free(op);
}

////////////////////////////////////////////////////////////////////////////////
// Debug printer
////////////////////////////////////////////////////////////////////////////////

static void _print_indent(int indent)
{
    for (int i = 0; i < indent; i++) {
        printf("  ");
    }
}

////////////////////////////////////////////////////////////////////////////////
/// Prints one <data> leaf as a single line, indented by `indent` two-space
/// levels -- the shape the document itself uses.
///
/// @param d      The leaf to print.
/// @param indent How many levels to indent it by.
static void _print_data(const hes_gw_data_t* d, int indent)
{
    _print_indent(indent);
    printf("<data transCode=\"%s\" descriptiveName=\"%s\">%s</data>\n", d->trans_code,
           d->descriptive_name, d->value);
}

////////////////////////////////////////////////////////////////////////////////
/// Prints a list of <data> leaves, one per line, in document order.
///
/// @param data   The leaves to print.
/// @param count  How many there are.
/// @param indent How many levels to indent them by.
static void _print_data_list(const hes_gw_data_t* const* data, size_t count, int indent)
{
    for (size_t i = 0; i < count; i++) {
        _print_data(data[i], indent);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

hes_gateway_t* hes_gateway_load(const char* filepath)
{
    if (filepath == NULL) {
        return NULL;
    }

    hes_query_ctx_t* ctx = hes_query_open(filepath);
    if (ctx == NULL) {
        return NULL;
    }

    hes_gateway_t* gw = (hes_gateway_t*)calloc(1, sizeof(*gw));
    if (gw == NULL) {
        hes_query_close(ctx);
        return NULL;
    }

    // <file> metadata leaves -- plain text children of the root element
    gw->file_format = _str(hes_query_string(ctx, "file.fileFormat"));
    gw->file_type = _str(hes_query_string(ctx, "file.fileType"));
    gw->file_purpose = _str(hes_query_string(ctx, "file.filePurpose"));
    gw->file_manu = _str(hes_query_string(ctx, "file.fileManu"));
    gw->file_product = _str(hes_query_string(ctx, "file.fileProduct"));
    gw->file_product_ref_id = _str(hes_query_string(ctx, "file.fileProductRefID"));
    gw->file_coverage = _str(hes_query_string(ctx, "file.fileCoverage"));
    gw->file_status = _str(hes_query_string(ctx, "file.fileStatus"));
    gw->file_version = _str(hes_query_string(ctx, "file.fileVersion"));
    gw->file_summary_description = _str(hes_query_string(ctx, "file.fileSummaryDescription"));
    gw->file_created_description = _str(hes_query_string(ctx, "file.fileCreatedDescription"));
    gw->file_created_date = _str(hes_query_string(ctx, "file.fileCreatedDate"));
    gw->file_revision_description = _str(hes_query_string(ctx, "file.fileRevisionDescription"));
    gw->file_revision_date = _str(hes_query_string(ctx, "file.fileRevisionDate"));

    // process tree
    if (hes_query_count(ctx, "file.overallProcess") > 0) {
        gw->overall_process = _load_overall_process(ctx, "file.overallProcess");
    }

    hes_query_close(ctx);
    return gw;
}

////////////////////////////////////////////////////////////////////////////////
// Lookup helpers
////////////////////////////////////////////////////////////////////////////////

const hes_gw_data_t* hes_gw_data_find(hes_gw_data_t* const* list, const char* descriptive_name)
{
    if (list == NULL || descriptive_name == NULL) {
        return NULL;
    }

    for (size_t i = 0; list[i] != NULL; i++) {
        if (list[i]->descriptive_name != NULL &&
            strcmp(list[i]->descriptive_name, descriptive_name) == 0) {
            return list[i];
        }
    }

    return NULL;
}

const char* hes_gw_data_value(hes_gw_data_t* const* list, const char* descriptive_name)
{
    const hes_gw_data_t* d = hes_gw_data_find(list, descriptive_name);
    return (d != NULL) ? d->value : NULL;
}

void hes_gateway_free(hes_gateway_t* gw)
{
    if (gw == NULL) {
        return;
    }

    free(gw->file_format);
    free(gw->file_type);
    free(gw->file_purpose);
    free(gw->file_manu);
    free(gw->file_product);
    free(gw->file_product_ref_id);
    free(gw->file_coverage);
    free(gw->file_status);
    free(gw->file_version);
    free(gw->file_summary_description);
    free(gw->file_created_description);
    free(gw->file_created_date);
    free(gw->file_revision_description);
    free(gw->file_revision_date);
    hes_gw_overall_process_free(gw->overall_process);
    free(gw);
}

void hes_gateway_print(const hes_gateway_t* gw)
{
    if (gw == NULL) {
        return;
    }

    printf("<file>\n");
    _print_indent(1);
    printf("<fileFormat>%s</fileFormat>\n", gw->file_format);
    _print_indent(1);
    printf("<fileType>%s</fileType>\n", gw->file_type);
    _print_indent(1);
    printf("<filePurpose>%s</filePurpose>\n", gw->file_purpose);
    _print_indent(1);
    printf("<fileManu>%s</fileManu>\n", gw->file_manu);
    _print_indent(1);
    printf("<fileProduct>%s</fileProduct>\n", gw->file_product);
    _print_indent(1);
    printf("<fileProductRefID>%s</fileProductRefID>\n", gw->file_product_ref_id);
    _print_indent(1);
    printf("<fileCoverage>%s</fileCoverage>\n", gw->file_coverage);
    _print_indent(1);
    printf("<fileStatus>%s</fileStatus>\n", gw->file_status);
    _print_indent(1);
    printf("<fileVersion>%s</fileVersion>\n", gw->file_version);
    _print_indent(1);
    printf("<fileSummaryDescription>%s</fileSummaryDescription>\n", gw->file_summary_description);
    _print_indent(1);
    printf("<fileCreatedDescription>%s</fileCreatedDescription>\n", gw->file_created_description);
    _print_indent(1);
    printf("<fileCreatedDate>%s</fileCreatedDate>\n", gw->file_created_date);
    _print_indent(1);
    printf("<fileRevisionDescription>%s</fileRevisionDescription>\n",
           gw->file_revision_description);
    _print_indent(1);
    printf("<fileRevisionDate>%s</fileRevisionDate>\n", gw->file_revision_date);

    if (gw->overall_process != NULL) {
        const hes_gw_overall_process_t* op = gw->overall_process;
        printf("  <overallProcess transCode=\"%s\" descriptiveName=\"%s\">\n", op->trans_code,
               op->descriptive_name);
        for (size_t l = 0; l < op->lexicon_type_count; l++) {
            const hes_gw_lexicon_type_t* lx = op->lexicon_types[l];
            printf("    <lexiconType transCode=\"%s\" descriptiveName=\"%s\">\n", lx->trans_code,
                   lx->descriptive_name);
            for (size_t o = 0; o < lx->object_type_count; o++) {
                const hes_gw_object_type_t* obj = lx->object_types[o];
                printf("      <objectType transCode=\"%s\" descriptiveName=\"%s\">\n",
                       obj->trans_code, obj->descriptive_name);
                _print_data_list((const hes_gw_data_t* const*)obj->data, obj->data_count, 7);
                for (size_t t = 0; t < obj->operation_table_count; t++) {
                    const hes_gw_operation_table_t* ot = obj->operation_tables[t];
                    printf("        <operationTable transCode=\"%s\" descriptiveName=\"%s\">\n",
                           ot->trans_code, ot->descriptive_name);
                    _print_data_list((const hes_gw_data_t* const*)ot->data, ot->data_count, 9);
                    for (size_t i = 0; i < ot->inputs_count; i++) {
                        const hes_gw_inputs_t* in = ot->inputs[i];
                        printf("          <inputs transCode=\"%s\" "
                               "descriptiveName=\"%s\">\n",
                               in->trans_code, in->descriptive_name);
                        _print_data_list((const hes_gw_data_t* const*)in->data, in->data_count, 11);
                        printf("          </inputs>\n");
                    }
                    for (size_t i = 0; i < ot->input_parameters_count; i++) {
                        const hes_gw_param_group_t* g = ot->input_parameters[i];
                        printf("          <inputParameters transCode=\"%s\" "
                               "descriptiveName=\"%s\">\n",
                               g->trans_code, g->descriptive_name);
                        _print_data_list((const hes_gw_data_t* const*)g->data, g->data_count, 11);
                        printf("          </inputParameters>\n");
                    }
                    for (size_t i = 0; i < ot->output_parameters_count; i++) {
                        const hes_gw_param_group_t* g = ot->output_parameters[i];
                        printf("          <outputParameters transCode=\"%s\" "
                               "descriptiveName=\"%s\">\n",
                               g->trans_code, g->descriptive_name);
                        _print_data_list((const hes_gw_data_t* const*)g->data, g->data_count, 11);
                        printf("          </outputParameters>\n");
                    }
                    for (size_t i = 0; i < ot->outputs_count; i++) {
                        const hes_gw_outputs_t* out = ot->outputs[i];
                        printf("          <outputs transCode=\"%s\" "
                               "descriptiveName=\"%s\">\n",
                               out->trans_code, out->descriptive_name);
                        _print_data_list((const hes_gw_data_t* const*)out->data, out->data_count,
                                         11);
                        printf("          </outputs>\n");
                    }
                    printf("        </operationTable>\n");
                }
                for (size_t t = 0; t < obj->addressing_table_count; t++) {
                    const hes_gw_addressing_table_t* at = obj->addressing_tables[t];
                    printf("        <addressingTable transCode=\"%s\" descriptiveName=\"%s\">\n",
                           at->trans_code, at->descriptive_name);
                    _print_data_list((const hes_gw_data_t* const*)at->data, at->data_count, 9);
                    printf("        </addressingTable>\n");
                }
                printf("      </objectType>\n");
            }
            printf("    </lexiconType>\n");
        }
        printf("  </overallProcess>\n");
    }
    printf("</file>\n");
}
