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

#ifndef OPENHES_SRC_COMMON_HES_GATEWAY_H
#define OPENHES_SRC_COMMON_HES_GATEWAY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

////////////////////////////////////////////////////////////////////////////////
/// @file
/// @brief Native C struct mapping of the `<file>` binding-map XML (Pattern 1),
/// built through the hes_query.h path-query layer (Pattern 3).
///
/// @details
/// The <file> binding-map XML is parsed ONCE (via the Pattern 3 path-query API
/// in hes_query.h) into a tree of native C structs.  After hes_gateway_load()
/// returns you navigate the tree directly with normal C syntax.
///
/// Lists (elements that can appear more than once under the same parent) use
/// Approach A: pointer-to-pointer arrays, e.g.
///
///     hes_gw_operation_table_t **operation_tables;
///     size_t                    operation_table_count;
///
/// Every list is NULL-terminated (a trailing NULL pointer, like a C string), so
/// it can be iterated without a count; the count field is kept for O(1) size
/// access and indexed loops. The loader sets both in a single pass.
///
/// Every _free() function mirrors its matching loader so ownership is always
/// explicit.  String fields are strdup()'d; missing elements yield "" so you
/// never dereference NULL.
///
/// Memory/ownership: the tree and every string in it belong to the caller. Both
/// are allocated by hes_gateway_load() (or its _load_file() wrapper) and released
/// only by the matching _free() functions.
///
/// Threading: no shared state -- one loaded tree is read by whoever owns it.

////////////////////////////////////////////////////////////////////////////////
/// A single <data> leaf element (identified by transCode / descriptiveName).
typedef struct hes_gw_data {
    char* version_number;    ///< XML @versionNumber, e.g. "1.0.0"
    char* addr_point;        ///< XML @addrPoint, the addressing point
    char* trans_code;        ///< XML @transCode, the datum's short name, e.g. "di"
    char* descriptive_name;  ///< XML @descriptiveName, e.g. "deviceIndex"
    char* data_purpose;      ///< XML @dataPurpose
    char* data_format;       ///< XML @dataFormat, the declared value type
    char* memory_type;       ///< XML @memoryType: pr/ro/op/sp/po
    char* storage_type;      ///< XML @storageType
    char* enumerated;        ///< XML @enumerated, the value list, if any
    char* description;       ///< XML @description
    char* preassigned;       ///< XML @preassigned, a fixed value, if any
    char* value;             ///< text content of the <data> element
} hes_gw_data_t;

////////////////////////////////////////////////////////////////////////////////
/// <inputs> element: a named group of <data> leaves.
typedef struct hes_gw_inputs {
    char* version_number;    ///< XML @versionNumber
    char* addr_point;        ///< XML @addrPoint
    char* trans_code;        ///< XML @transCode, e.g. "ip1"
    char* descriptive_name;  ///< XML @descriptiveName, "inputs"
    char* data_purpose;      ///< XML @dataPurpose
    hes_gw_data_t** data;    ///< the <data> leaves, in document order
    size_t data_count;       ///< number of entries in @ref data
} hes_gw_inputs_t;

////////////////////////////////////////////////////////////////////////////////
/// <outputs> element: a named group of <data> leaves.
typedef struct hes_gw_outputs {
    char* version_number;    ///< XML @versionNumber
    char* addr_point;        ///< XML @addrPoint
    char* trans_code;        ///< XML @transCode, e.g. "op1"
    char* descriptive_name;  ///< XML @descriptiveName, "outputs"
    char* data_purpose;      ///< XML @dataPurpose
    hes_gw_data_t** data;    ///< the <data> leaves, in document order
    size_t data_count;       ///< number of entries in @ref data
} hes_gw_outputs_t;

////////////////////////////////////////////////////////////////////////////////
/// <inputParameters> / <outputParameters> element.
typedef struct hes_gw_param_group {
    char* version_number;    ///< XML @versionNumber
    char* addr_point;        ///< XML @addrPoint
    char* trans_code;        ///< XML @transCode, e.g. "pi1"
    char* descriptive_name;  ///< XML @descriptiveName
    char* data_purpose;      ///< XML @dataPurpose
    char* description;       ///< XML @description
    hes_gw_data_t** data;    ///< the <data> leaves, in document order
    size_t data_count;       ///< number of entries in @ref data
} hes_gw_param_group_t;

////////////////////////////////////////////////////////////////////////////////
/// <operationTable> element.
typedef struct hes_gw_operation_table {
    char* version_number;    ///< XML @versionNumber
    char* addr_point;        ///< XML @addrPoint
    char* trans_code;        ///< XML @transCode, e.g. "ot1"
    char* descriptive_name;  ///< XML @descriptiveName, "operationTable"
    char* data_purpose;      ///< XML @dataPurpose

    /// direct <data> children: refId, enable, operation, ...
    hes_gw_data_t** data;
    size_t data_count;  ///< number of entries in @ref data

    /// <inputs> groups (a table may hold more than one)
    hes_gw_inputs_t** inputs;
    size_t inputs_count;  ///< number of entries in @ref inputs

    /// <inputParameters> groups
    hes_gw_param_group_t** input_parameters;
    size_t input_parameters_count;  ///< number of entries in @ref input_parameters

    /// <outputParameters> groups
    hes_gw_param_group_t** output_parameters;
    size_t output_parameters_count;  ///< number of entries in @ref output_parameters

    /// <outputs> groups
    hes_gw_outputs_t** outputs;
    size_t outputs_count;  ///< number of entries in @ref outputs
} hes_gw_operation_table_t;

////////////////////////////////////////////////////////////////////////////////
/// <addressingTable> element.
typedef struct hes_gw_addressing_table {
    char* version_number;    ///< XML @versionNumber
    char* addr_point;        ///< XML @addrPoint
    char* trans_code;        ///< XML @transCode, e.g. "at1"
    char* descriptive_name;  ///< XML @descriptiveName, "addressingTable"
    char* data_purpose;      ///< XML @dataPurpose

    /// direct <data> children: deviceIndex, moduleType, moduleRefIndex,
    /// netRefIndex, address
    hes_gw_data_t** data;
    size_t data_count;  ///< number of entries in @ref data
} hes_gw_addressing_table_t;

////////////////////////////////////////////////////////////////////////////////
/// <objectType> element.
typedef struct hes_gw_object_type {
    char* version_number;    ///< XML @versionNumber
    char* addr_point;        ///< XML @addrPoint
    char* trans_code;        ///< XML @transCode, e.g. "bm"
    char* descriptive_name;  ///< XML @descriptiveName, e.g. "bindingMap"

    /// direct <data> children: requirementMandatory, single
    hes_gw_data_t** data;
    size_t data_count;  ///< number of entries in @ref data

    hes_gw_operation_table_t** operation_tables;  ///< <operationTable> children
    size_t operation_table_count;                 ///< entries in @ref operation_tables

    hes_gw_addressing_table_t** addressing_tables;  ///< <addressingTable> children
    size_t addressing_table_count;                  ///< entries in @ref addressing_tables
} hes_gw_object_type_t;

////////////////////////////////////////////////////////////////////////////////
/// <lexiconType> element.
typedef struct hes_gw_lexicon_type {
    char* version_number;    ///< XML @versionNumber
    char* addr_point;        ///< XML @addrPoint
    char* trans_code;        ///< XML @transCode, e.g. "ob"
    char* descriptive_name;  ///< XML @descriptiveName, e.g. "object"

    hes_gw_object_type_t** object_types;  ///< <objectType> children
    size_t object_type_count;             ///< entries in @ref object_types
} hes_gw_lexicon_type_t;

////////////////////////////////////////////////////////////////////////////////
/// <overallProcess> element.
typedef struct hes_gw_overall_process {
    char* version_number;    ///< XML @versionNumber
    char* addr_point;        ///< XML @addrPoint
    char* trans_code;        ///< XML @transCode, "lx"
    char* descriptive_name;  ///< XML @descriptiveName, "lexicon"

    hes_gw_lexicon_type_t** lexicon_types;  ///< <lexiconType> children
    size_t lexicon_type_count;              ///< entries in @ref lexicon_types
} hes_gw_overall_process_t;

////////////////////////////////////////////////////////////////////////////////
/// Root struct: one <file> element.  All the file metadata plus the parsed
/// process tree.  This is the struct you keep in the application and navigate
/// to obtain anything from the XML.
typedef struct hes_gateway {
    /// <file> metadata, one field per element of the header
    char* file_format;                ///< XML file.fileFormat
    char* file_type;                  ///< XML file.fileType
    char* file_purpose;               ///< XML file.filePurpose
    char* file_manu;                  ///< XML file.fileManu, the manufacturer
    char* file_product;               ///< XML file.fileProduct
    char* file_product_ref_id;        ///< XML file.fileProductRefID
    char* file_coverage;              ///< XML file.fileCoverage
    char* file_status;                ///< XML file.fileStatus
    char* file_version;               ///< XML file.fileVersion
    char* file_summary_description;   ///< XML file.fileSummaryDescription
    char* file_created_description;   ///< XML file.fileCreatedDescription
    char* file_created_date;          ///< XML file.fileCreatedDate
    char* file_revision_description;  ///< XML file.fileRevisionDescription
    char* file_revision_date;         ///< XML file.fileRevisionDate

    /// process tree (single <overallProcess> under <file>)
    hes_gw_overall_process_t* overall_process;
} hes_gateway_t;

////////////////////////////////////////////////////////////////////////////////
// Loading / lifetime
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// Parse an HES binding-map XML file into native structs.
/// Uses the Pattern 3 path-query API internally to fill every field.
///
/// @param filepath Path to the XML file
/// @return Newly allocated root struct, or NULL on failure.
hes_gateway_t* hes_gateway_load(const char* filepath);

////////////////////////////////////////////////////////////////////////////////
/// Free the root struct and everything it owns (deep free).
void hes_gateway_free(hes_gateway_t* gw);

////////////////////////////////////////////////////////////////////////////////
// Lookup helpers
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// Find a <data> entry by its descriptiveName (e.g. "refId", "operation",
/// "deviceIndex"). Returns NULL when not found. `list` is the NULL-terminated
/// data array of a container, so no size argument is needed.
const hes_gw_data_t* hes_gw_data_find(hes_gw_data_t* const* list, const char* descriptive_name);

////////////////////////////////////////////////////////////////////////////////
/// Value of the <data> entry with the given descriptiveName, or NULL.
/// Convenience wrapper around hes_gw_data_find().
const char* hes_gw_data_value(hes_gw_data_t* const* list, const char* descriptive_name);

////////////////////////////////////////////////////////////////////////////////
/// Pretty-print the whole tree (useful for debugging).
void hes_gateway_print(const hes_gateway_t* gw);

#ifdef __cplusplus
}
#endif

#endif  // #ifndef OPENHES_SRC_COMMON_HES_GATEWAY_H
