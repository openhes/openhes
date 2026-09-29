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
/// @brief Binding map service: the sole gateway from a service module to
/// HES-CLME (ISO/IEC 18012-3 Annex A.1; ISO/IEC 15045-4-1 A.3/A.4).
///
/// @details
/// Per ISO/IEC 18012-3 Annex A.1: "The only way that a service module or a HES
/// gateway app can access the HES-CLME is through its binding map." This module
/// implements that binding map with the two roles the standard describes, both
/// exposed by the public API below:
///
///   - "controller" service: setup/configuration -- reads the XML description
///     (addressingTable + operationTable) and, at startup, tells every external
///     module it depends on to subscribe/publish the objects the operation table
///     needs (bm_controller_start()).
///   - "processor" service: real-time operation -- receives
///     event-report/get-response messages, evaluates the operation table's rules
///     (a small dataflow graph), and emits "put" messages to whatever the rules'
///     outputs point at (bm_processor_handle()).
///
/// Per the 18012-3 note "the conversions can be effectively null", a rule with
/// no real conversion is still a first-class rule here (see the example XML's
/// boolean-flag chaining through 'it' devices).
///
/// XML shape parsed here matches the SC25-WG1 18012-3 binding-map sample
/// instance document: an <objectType transCode="bm"> containing one or more
/// <operationTable> and <addressingTable> elements.

#ifndef OPENHES_SRC_BM_BM_H
#define OPENHES_SRC_BM_BM_H

#include "common/hes_bus.h"
#include "common/hes_common.h"

#define BM_MAX_OPS 32
#define BM_MAX_INPUTS 4
#define BM_MAX_ADDRS 64

////////////////////////////////////////////////////////////////////////////////
/// One entry of an <inputs> element: a source to compare/read.
typedef struct bm_input {
    uint32_t device_index;  ///< 'di'

    /// 'so', may be empty when the input is really just the device_index's
    /// cached value (e.g. an internal 'it' flag)
    char source_object[HES_PATH_MAX];

    /// 'co': pt (pass through), bk (block), av (abs value), ng (invert) --
    /// applied to this operand before it's combined with the others
    char condition[4];

    /// 'at': bk (block) | fl (flow) | pa (partial) -- 18012-3 Table 134.
    /// 'bk' means this input may not drive the row at all. Empty defaults to 'fl'.
    char author_type[4];

    /// 'ap': pt (pass through) | bk (block) | au (authorize) -- what to do with
    /// the message's accompanied credential ('ci'/'un'). Read by the per-message
    /// authorization path; see src/services/auth/README.md.
    char accompany_op[4];
} bm_input_t;

////////////////////////////////////////////////////////////////////////////////
/// One evaluated operand handed to the app service (op="ap"). The built-in
/// operators take at most two operands, but 18012-3 does not bound how many an
/// appService sees, so the app receives every input of the row.
typedef struct bm_operand {
    uint32_t device_index;  ///< the input's 'di'

    /// the input's 'so' ("" for an internal input or a preset literal)
    char source_object[HES_PATH_MAX];

    double value;  ///< the conditioned operand value
} bm_operand_t;

////////////////////////////////////////////////////////////////////////////////
/// One row of the operation table (one <operationTable>/otN element).
typedef struct bm_operation {
    uint32_t ref_id;    ///< 'ri'
    int enabled;        ///< 'en' == "en"
    char operation[4];  ///< 'op': gt, lt, eq, ad, su, mu, dv, ...

    /// operationTable transCode, e.g. "ot1" -- needed to form this row's
    /// binding-map address; see bm_at_address()
    char table_code[8];

    bm_input_t inputs[BM_MAX_INPUTS];  ///< the row's <inputs> entries
    int n_inputs;                      ///< number of entries in @ref inputs

    /// inputParameters: either a literal comparison value, or the sentinel
    /// meaning "use the live value of this input's object" (the sample XML uses
    /// the string "va" for that -- see ip_is_live[] below).
    double input_params[BM_MAX_INPUTS];
    int ip_is_live[BM_MAX_INPUTS];  ///< 1 when input_params[i] means "read live"
    int n_input_params;             ///< number of entries in @ref input_params

    /// 'op' under outputParameters: which branch (true/false) maps to which
    /// output device -- simplified to a single boolean result in this POC
    int output_params[2];
    int n_output_params;  ///< number of entries in @ref output_params

    /// outputs/'dl': deviceList index of the result -- POC supports one output
    uint32_t out_device_index;

    /// outputs/'do': destObject, may be empty for an internal-only ('it') result
    char out_dest_object[HES_PATH_MAX];

    char out_method[4];  ///< outputs/'me': "up" (update) etc.

    /// outputs/'ad': pt/bk/av/ng -- applied to the evaluated result before it's
    /// cached/sent
    char out_adjustment[4];

    /// outputs transCode, e.g. "op1" -- part of the row's authorType address
    char out_code[8];

    /// outputs/'at': bk (block) | fl (flow) | pa (partial) -- 18012-3 Table 135.
    /// Written by the authorization service (bm_set_at) and enforced before a PUT
    /// is emitted. Empty defaults to 'fl'.
    char out_author_type[4];

    /// Change-detection so we only re-send a PUT when the evaluated result
    /// actually changes, instead of flooding the bus.
    int has_last_output;
    double last_output_value;

    /// Change-detection on the OPERANDS. The table is re-evaluated recursively
    /// after every result is cached, so without this a row would be recomputed
    /// (and, for op="ap", the app re-invoked) with the very same inputs. That
    /// matters because an appService may be stateful. The result check above
    /// still prevents duplicate PUTs when the operands change but the result
    /// does not. Every operand is tracked, not just the first two, so a change
    /// in any input of an appService row reaches the app.
    int has_last_inputs;
    int n_last_operands;
    double last_operands[BM_MAX_INPUTS];
} bm_operation_t;

////////////////////////////////////////////////////////////////////////////////
/// One row of the addressing table (one <addressingTable>/atN element).
typedef struct bm_addr {
    uint32_t device_index;          ///< 'di'
    hes_module_type_t module_type;  ///< 'mt': hi/wi/sm/it
    uint32_t module_ref_index;      ///< 'mi'
    uint32_t net_ref_index;         ///< 'ni'
    char address[64];               ///< 'ad': IP address, may be empty
} bm_addr_t;

typedef struct binding_map {
    bm_operation_t ops[BM_MAX_OPS];  ///< the operation rows, in XML order
    int n_ops;                       ///< number of rows in @ref ops

    bm_addr_t addrs[BM_MAX_ADDRS];  ///< the addressing-table rows, in XML order
    int n_addrs;                    ///< number of rows in @ref addrs

    /// Runtime value cache, keyed by device_index (1-based; index 0 unused).
    double value_cache[BM_MAX_ADDRS + 1];  ///< last value seen for that device
    int value_valid[BM_MAX_ADDRS + 1];     ///< 1 when that cache entry is meaningful

    /// Optional app-service bridge -- how the customer-specific protected
    /// app (the "Lua app") takes part in the dataflow. The engine does not
    /// know what an "app" is: the host wires this in (see
    /// src/modules/core/core.c), which keeps bm.c Lua-free and matches
    /// 18012-3 Annex A.1, where the binding map exchanges data with the app
    /// attached to it "either directly or through service objects".
    ///
    /// Optional; NULL means every row uses the built-in operation set.
    ///
    /// NOTE: the app is reached ONLY through the 'ap' operation code
    /// (appService). There is deliberately no "the app is a destination"
    /// hook: moduleType 'it' already means "internal process" and is used
    /// for ordinary internal chaining values, so overloading it to mean
    /// "the app" would make the two indistinguishable in XML. See
    /// README.md, "Routing data through the Lua app".
    ///
    /// A row whose 'op' is "ap" (appService) is dispatched here instead of to
    /// the built-in operation set. The built-in operators take at most two
    /// operands, but 18012-3 does not bound how many operands an appService
    /// sees, so ALL of the row's conditioned inputs are passed. The app writes
    /// its numeric result to *result.
    ///
    /// Returns 0 on success, non-zero if the operation could not be performed
    /// (no handler registered); the row is then skipped with a diagnostic.
    int (*app_operation)(void* ctx,
                         const bm_operation_t* op,
                         const bm_operand_t* operands,
                         int n_operands,
                         double* result);
    void* app_ctx;  ///< opaque host pointer handed back to the hook above

    /// Development switch: 1 = ignore EVERY authorization gate, whatever the
    /// XML declares or the policy writes. Input authorType, output authorType
    /// and the per-message 'au' check below are all skipped, so no row can be
    /// suppressed on authorization grounds. Set once, by
    /// bm_disable_authorization(); never cleared.
    ///
    /// Deliberately a property of the whole map rather than of a row or a
    /// message: "authorization is off" has to be total, or some gate would
    /// still refuse the traffic it was meant to let through.
    int authz_disabled;

    /// Mode B -- per-message authorization (18012-3 Table 134).
    ///
    /// When a row's input declares accompanyOperation ('ap') = "au", the
    /// accompanied information on the incoming message (ci credInfo / un
    /// userName) must be authorized before that row may output. This is needed
    /// when several senders share one row: 'at' is a property of the row and
    /// cannot vary per sender, so the decision has to be made per message.
    ///
    /// The host wires this to the A&A service; bm.c stays free of any service
    /// knowledge. Returns 1 when authorized, 0 to suppress the output. NULL
    /// means every 'au' row is denied (fail closed).
    int (*auth_authorize)(void* ctx, const hes_clme_msg_t* msg, const char* at_address);
    void* auth_ctx;  ///< opaque host pointer handed back to the hook above

    /// The message currently being processed. Needed because authorization is
    /// per message while evaluate_op() is also reached recursively through
    /// bm_evaluate_all(). Set by bm_processor_handle(); may be NULL.
    const hes_clme_msg_t* current_msg;
} binding_map_t;

void bm_init(binding_map_t* bm);

////////////////////////////////////////////////////////////////////////////////
/// Parses a binding-map XML instance document (libxml2) into *bm.
/// Returns 0 on success, -1 on error (message printed to stderr).
///
/// @param bm The binding map to populate.
/// @param filename The XML file to read.
/// @return 0 on success, -1 on error.
int bm_load_xml(binding_map_t* bm, const char* filename);

////////////////////////////////////////////////////////////////////////////////
/// Looks up an addressing-table row by device_index. Returns NULL if absent.
///
/// @param bm The binding map to search.
/// @param device_index The device index to look up.
/// @return A pointer to the addressing-table row, or NULL if not found.
const bm_addr_t* bm_find_addr(const binding_map_t* bm, uint32_t device_index);

////////////////////////////////////////////////////////////////////////////////
/// Is (path, device_index) a destination this binding map may relay a
/// command to? True when path is non-empty and device_index resolves to an
/// addressing-table row whose module type is 'hi' or 'wi' -- i.e. a real
/// device behind a HAN/WAN interface module. 'sm'/'it' targets are refused.
///
/// A device-level question by design, and the path is not matched against the
/// operation table: which objects the device really has, and whether a command
/// may write them, is the interface module's business (its manifest slice and
/// the object's access), not this map's.
///
/// Used by the core to pass an external module's PUT downstream without
/// turning the core into a blind proxy: the row above is the whole test, so a
/// command can only reach a device the map already declares.
///
/// @param bm The binding map to check.
/// @param path The lexicon address of the command.
/// @param device_index The device_index of the command's destination.
/// @return 1 if the destination is declared, 0 if not.
int bm_is_declared_destination(const binding_map_t* bm, const char* path, uint32_t device_index);

////////////////////////////////////////////////////////////////////////////////
/// Builds the binding-map address of this row's authorType output field, e.g.
/// "/lx/ob/bm/ot1/op1/at" -- the address that permissionService rows point at
/// through their lexiconObject ('lo').
///
/// @param op The operation-table row to address.
/// @param buf The buffer to write the address into.
/// @param buf_size The capacity of buf.
void bm_at_address(const bm_operation_t* op, char* buf, size_t buf_size);

////////////////////////////////////////////////////////////////////////////////
/// Sets the authorType ('at') of the row addressed by at_address. This is how the
/// authorization service reaches into a binding map: an in-process call when
/// both live in the same module (see core.c), or a PUT to the same address when
/// the binding map lives in another module.
///
/// @param bm The binding map to modify.
/// @param at_address The binding-map address of the row's authorType field.
/// @param value The new value to write into that row's authorType field.
/// @return 0 on success, -1 if no row carries that address.
int bm_set_at(binding_map_t* bm, const char* at_address, const char* value);

////////////////////////////////////////////////////////////////////////////////
/// Turns authorization off for this binding map -- the core module's
/// --no-authz development switch.
///
/// From then on no row can be suppressed on authorization grounds: input
/// authorType ('at'), output authorType ('at') and the per-message 'ap'='au'
/// hook are all ignored, whatever the XML declares or the policy writes.
///
/// bm_set_at() still updates the rows, so a host that keeps propagating a
/// policy can still see what authorization *would* have done; the fields simply
/// stop deciding anything.
///
/// One-way on purpose. This exists to take authorization out of the way while
/// investigating a problem, a demo or a bisect -- not to toggle it at runtime,
/// which would need a story for what happens to a gate that was already armed.
///
/// @param bm The binding map to disable authorization on.
void bm_disable_authorization(binding_map_t* bm);

////////////////////////////////////////////////////////////////////////////////
/// The numeric value of one datum in a bus payload.
///
/// A service object answers with ';'-separated 'transCode=value' records (the
/// identification and time services both do), and the operation table's
/// inputParameter says which datum matters: its 'va' means "the value of
/// currentValue" (18012-3 Table 26). An interface module, by contrast, puts a
/// bare value on the bus ("1", "23.5"), so a payload with no '=' in it is read
/// as the value itself.
///
/// @param payload The bus message's payload string.
/// @param trans_code The transCode of the datum to read, or NULL to read the
///                  payload as a bare value.
/// @return The numeric value of that datum, or 0.0 if the datum is absent or the payload is NULL.
double bm_datum_value(const char* payload, const char* trans_code);

////////////////////////////////////////////////////////////////////////////////
/// Binding-map controller service: the setup/configuration half of the binding
/// map (see ISO/IEC 15045-4-1 Annex A.3 and ISO/IEC 18012-3 Clause 10). Called
/// once at startup, after the bus is up and the XML has been loaded with
/// bm_load_xml().
///
/// It walks every ENABLED operation row and, for each EXTERNAL input (a source
/// object owned by a HAN/WAN interface or service module), sends one HES-CLME
/// SUBSCRIBE on the bus so that object's currentValue event-reports begin
/// flowing back to the processor below. This is the "wire up the data sources"
/// step; the runtime matching, conversion and dispatch is bm_processor_handle()
/// / evaluate_op().
///
/// Inputs with no external publisher are skipped:
///   - source_object is empty   -> chained 'it'/internal value, produced
///                                 by another operation's output in this
///                                 same table (nothing to subscribe to);
///   - already in sent[]        -> one subscription per distinct
///                                 (path, deviceIndex) pair, however many
///                                 rows reference it;
///   - no addressing-table row, -> no external module to subscribe to.
///     or module_type 'it'/NONE
///
/// @param bm  the loaded binding map
/// @param bus the bus to subscribe on
void bm_controller_start(binding_map_t* bm, hes_bus_t* bus);

////////////////////////////////////////////////////////////////////////////////
/// Binding-map processor service: feed every inbound bus message here.
/// Internally updates the value cache, re-evaluates any operations that became
/// ready, and sends "put" messages for whatever the rules produce.
///
/// @param bm The binding map to evaluate.
/// @param bus The bus to send any resulting "put" messages on.
/// @param in The inbound message to process.
void bm_processor_handle(binding_map_t* bm, hes_bus_t* bus, const hes_clme_msg_t* in);

#endif  // #ifndef OPENHES_SRC_BM_BM_H
