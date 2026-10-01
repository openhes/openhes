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
/// @brief The core service module (the hub): the gateway's binding map, service
/// objects, Lexicon router and customer-specific protected app.
///
/// @details
/// Contains, per ISO/IEC 15045-4-1 Annex A.4 / A.3:
///   - a binding map (mandatory; the sole gateway onto HES-CLME)
///   - zero or more service objects: here, identification, time,
///     authorization/authentication, and cryptographic services (all
///     "foundational" per ISO/IEC 18012-3)
///   - a customer-specific protected app (a Lua script)
///
/// Bus role: this module is the HUB (see common/hes_bus.h) -- it hosts
/// both the downstream PUB endpoint (pub_url) that HAN/WAN modules
/// dial into as subscribers, filtered to only the Lexicon paths they
/// own, and the upstream SUB endpoint (sub_url) that HAN/WAN modules
/// dial into as publishers to send their events. The hub itself
/// subscribes to everything upstream, since the binding map is the
/// one place the standard wants to see all HES-CLME traffic.
///
/// Memory/ownership: the module's state is local to core_main() -- the binding
/// map, the service objects and the app bridge are objects created there and
/// released on exit. The configuration strings are the caller's.
///
/// Threading: one event loop, plus the time service's NTP sync thread. The bus,
/// the binding map and the service objects are touched only from that loop.

#include "core.h"

#include "app_lua.h"
#include "bm/bm.h"
#include "common/hes_bus.h"
#include "common/hes_common.h"
#include "common/hes_dispatch.h"
#include "common/hes_mreg.h"
#include "common/service_object.h"
#include "services/auth/auth.h"
#include "services/crypto/crypto.h"
#include "services/id/id.h"
#include "services/time/time.h"
#include "services/time/time_sync.h"

#include <log.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_running = 1;
////////////////////////////////////////////////////////////////////////////////
/// SIGINT/SIGTERM handler: asks the main loop to stop by clearing g_running.
/// It does nothing else on purpose -- the loop then shuts down in order
/// (service objects, then the sync thread, then the bus), which a signal
/// handler could not do safely.
///
/// @param sig The signal number (unused).
static void on_sigint(int sig)
{
    (void)sig;
    g_running = 0;
}

/// Room for every local service object. The identification service contributes
/// one per Lexicon address it answers on (see src/services/id/id.h) and the time
/// service one per functional object (see src/services/time/time.h); with
/// authorization and cryptography that is 3 + 3 + 1 + 1 = 8 today, so 12 leaves
/// room for the next service without a silent overflow.
#define CORE_MAX_SERVICE_OBJECTS 12

////////////////////////////////////////////////////////////////////////////////
/// appService bridge -- a binding-map row with op="ap" asks the protected app
/// to perform the operation (18012-3 Table 26 / A.2.2). The row's inputs are
/// handed over in full, so the script can implement logic the built-in
/// operators cannot express (AND/OR of several conditions, thresholds, state).
///
/// @param ctx The app_lua_t instance registered in binding_map_t::app_ctx.
/// @param op The operation row that requested the app's service.
/// @param operands The row's conditioned inputs, in table order.
/// @param n_operands How many entries operands holds.
/// @param result Receives the app's numeric result.
/// @return 0 on success, non-zero when the app defines no handler or errored.
static int app_service_op(void* ctx,
                          const bm_operation_t* op,
                          const bm_operand_t* operands,
                          int n_operands,
                          double* result)
{
    if (app_lua_call_op((app_lua_t*)ctx, op->ref_id, operands, n_operands, result) != 0) {
        log_error("appService ri=%u not handled by the app", op->ref_id);
        return -1;
    }
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Writes one authorization decision into the binding map (see auth.h).
///
/// @param ctx The binding_map_t registered as the propagation target.
/// @param at_address Binding-map address, e.g. "/lx/ob/bm/ot1/op1/at".
/// @param value 'bk' | 'fl' | 'pa'.
/// @return 0 on success, non-zero when no row carries that address.
static int core_write_at(void* ctx, const char* at_address, const char* value)
{
    return bm_set_at((binding_map_t*)ctx, at_address, value);
}

////////////////////////////////////////////////////////////////////////////////
/// What the Mode B bridge needs in order to answer: the policy (the A&A service)
/// and the component that can check a proof (the crypto service).
///
/// The binding map only sees one opaque context, so the pairing lives here
/// rather than in binding_map_t -- the binding map has no business knowing that
/// a proof is a cryptographic object at all.
typedef struct core_authorizer {
    service_object_t* auth;
    service_object_t* crypto;
} core_authorizer_t;

////////////////////////////////////////////////////////////////////////////////
/// Mode B bridge -- a binding-map input marked ap='au' asks the A&A service to
/// authorize the accompanied information on the current message.
///
/// There are two gates, and a message must pass both:
///
///   1. the A&A service decides whether this class may act on this target at
///      all (Tables 60-68, the policy);
///   2. if the policy also marks the target as needing a proof, the message's
///      'ci' must carry a valid one.
///
/// The second gate is what turns "the gateway can prove itself" into "the
/// gateway demands proof". It is deliberately AFTER the first: an operation that
/// was never authorized is denied whatever else the message carries, so an
/// unauthorized sender learns nothing about the proof path.
///
/// @param ctx The core_authorizer_t registered in binding_map_t::auth_ctx.
/// @param msg The message being processed (carries 'ci' / 'un' / 'path').
/// @param at_address The authorType address of the row that wants to output.
/// @return 1 when authorized, 0 when denied.
static int core_authorize(void* ctx, const hes_clme_msg_t* msg, const char* at_address)
{
    const core_authorizer_t* authz = (const core_authorizer_t*)ctx;

    int rv = auth_service_authorize(authz->auth, msg->cred_info, msg->user_name, at_address);
    if (rv == 0) {
        return 0;
    }

    const char* expected_pi = auth_service_required_proof_pi(authz->auth, at_address);
    if (expected_pi == NULL) {
        return 1;  // this operation does not ask for a proof
    }

    // The proof signs the message's path and payload together, so a proof
    // copied from a different message will not verify here. Signing the payload
    // alone would let the same signature be reused at another address.
    char framed[HES_PATH_MAX + HES_PAYLOAD_MAX + 2];
    size_t framed_len = crypto_proof_message(msg->path, msg->payload, framed, sizeof(framed));
    if (crypto_verify_proof(msg->cred_info, expected_pi, (const unsigned char*)framed,
                            framed_len) != 1) {
        log_info("core: authorization denied (%s needs a valid proof)", at_address);
        return 0;
    }
    return 1;
}

////////////////////////////////////////////////////////////////////////////////
/// Counts how many distinct modules of one type the addressing table declares.
/// Several devices can sit behind one interface module, so rows are counted per
/// (moduleType, moduleRefIndex), not per row.
///
/// For example, tests/data/bm_appservice_multi_input.xml has four addressing rows:
///
///   di=6  mt=hi  mi=1   the SensorTag button
///   di=7  mt=hi  mi=1   its motion sensor   -- one BLE module, so these
///   di=8  mt=hi  mi=1   its light sensor    -- three rows count once
///   di=2  mt=hi  mi=2   the WiZ bulb, on the WiFi module
///
/// count_distinct_modules(bm, HES_MODTYPE_HI) therefore returns 2, not 4: the
/// three mi=1 rows are one module. Both fields must match, so a 'wi' row with
/// mi=1 is a different module from a 'hi' row with mi=1.
///
/// @param bm The binding map whose addressing table is inspected.
/// @param type 'hi' (HAN), 'wi' (WAN) or 'sm' (service module).
/// @return How many distinct moduleRefIndex values appear for that type.
static int count_distinct_modules(const binding_map_t* bm, hes_module_type_t type)
{
    uint32_t seen[BM_MAX_ADDRS];
    int n = 0;

    for (int i = 0; i < bm->n_addrs; i++) {
        if (bm->addrs[i].module_type != type) {
            continue;
        }

        uint32_t mi = bm->addrs[i].module_ref_index;
        int seen_before = 0;
        for (int j = 0; j < n; j++) {
            if (seen[j] == mi) {
                seen_before = 1;
                break;
            }
        }
        if (!seen_before) {
            seen[n++] = mi;
        }
    }
    return n;
}

/// How often the gateway's module list is re-read from the manifest service.
/// The roster of an integral gateway does not change while it runs, so this only
/// has to catch a module that was late to start.
#define CORE_INVENTORY_REFRESH_S 10

/// Reply timeout for a registry query: it is an in-memory answer on the same
/// machine, and a slow one must never hold up the bus loop.
#define CORE_INVENTORY_TIMEOUT_MS 500

////////////////////////////////////////////////////////////////////////////////
/// Where the identification service's inventory answers come from.
///
/// Preferred: the manifest service's module list, i.e. the product profile's own
/// inventory of this box. Table 42 counts the modules "present in the HES
/// gateway system", and a product profile declares exactly that -- so the
/// numbers no longer depend on any binding-map XML, which could only ever
/// describe one map's reach.
///
/// Fallback (no manifest service configured, or none reachable): the addressing
/// table we parsed, with this module counted by hand. That is a stand-in, not
/// the standard's answer -- see core_gateway_inventory().
typedef struct core_inventory {
    const binding_map_t* bm;  ///< fallback source: the addressing table
    const char* svc_rep_url;  ///< manifest service REQ URL; NULL = none configured
    hes_mreg_t mreg;
    int mreg_open;
    int have_modules;       ///< 1 once a module list has arrived
    hes_modules_t modules;  ///< the last successful answer
} core_inventory_t;

////////////////////////////////////////////////////////////////////////////////
/// Re-reads the gateway's module list, when a manifest service is configured.
///
/// Best effort by design: a service that is not up yet (or has gone away) leaves
/// the previous answer -- or the fallback -- in place, so a missing registry
/// degrades the counts but never breaks the identification service.
///
/// @param inv The inventory to refresh.
static void core_inventory_refresh(core_inventory_t* inv)
{
    if (inv->svc_rep_url == NULL || inv->svc_rep_url[0] == '\0') {
        return;
    }

    if (!inv->mreg_open) {
        if (hes_mreg_open_registry(&inv->mreg, inv->svc_rep_url, CORE_INVENTORY_TIMEOUT_MS) != 0) {
            return;  // not up yet: try again at the next refresh
        }
        inv->mreg_open = 1;
    }

    hes_modules_t modules = {0};
    if (hes_mreg_query_modules(&inv->mreg, &modules) != 0) {
        return;  // keep the last known list
    }

    inv->modules = modules;
    inv->have_modules = 1;
    log_debug("inventory: %d module(s) declared by the manifest service", modules.n);
}

////////////////////////////////////////////////////////////////////////////////
/// Answers the identification service's gateway-inventory question
/// (18012-3 Tables 39 and 42): how many interface modules the gateway has, and
/// where its binding maps are.
///
/// These are properties of the HES gateway system, not of one module, and the id
/// service cannot learn them by itself -- so the host answers. From the manifest
/// service it gets the product profile's module list, which is this box's own
/// inventory; without a manifest service it falls back to the addressing table
/// it has already parsed, which is where 15045-4-1 A.1/A.2 puts the gateway's
/// interface modules, and counts itself by hand.
///
/// @param ctx The core_inventory_t.
/// @param out Receives the counts and the binding-map rows.
static void core_gateway_inventory(void* ctx, id_inventory_t* out)
{
    const core_inventory_t* inv = (const core_inventory_t*)ctx;
    const binding_map_t* bm = inv->bm;

    memset(out, 0, sizeof(*out));

    if (inv->have_modules) {
        // The gateway's own roster. A service module hosts a binding map
        // (15045-4-1 A.4), so each 'sm' entry is both one of Table 42's counts
        // and one of Table 39's discovery rows -- hence one loop for both.
        for (int i = 0; i < inv->modules.n; i++) {
            const hes_module_entry_t* mod = &inv->modules.modules[i];

            if (strcmp(mod->module_type, "hi") == 0) {
                out->hans_number++;
            } else if (strcmp(mod->module_type, "wi") == 0) {
                out->wans_number++;
            } else if (strcmp(mod->module_type, "sm") == 0) {
                out->service_modules_number++;
                // 'ad' is an interface module's IP address, so it stays empty
                // for a service module (it has no network of its own -- it is
                // reached over HES-CLME).
                if (out->n_binding_maps < ID_MAX_BINDING_MAPS) {
                    id_binding_map_t* row = &out->binding_maps[out->n_binding_maps++];
                    hes_strlcpy(row->module_type, sizeof(row->module_type), "sm");
                    row->module_ref_index = mod->module_ref_index;
                    row->net_ref_index = 0;  // 0 = the module itself (Table 39)
                } else {
                    // 'ns' is still exact -- it has no cap -- but the discovery
                    // list is full. Say so, instead of answering Table 39 short
                    // without a word.
                    log_warn(
                            "inventory: %d service module(s) declared, but only %d discovery rows "
                            "fit",
                            out->service_modules_number, ID_MAX_BINDING_MAPS);
                }
            }
        }

        return;
    }

    // Fallback: no module list, so derive what we can from the addressing table
    // we parsed. The table is one binding map's reach list, so these numbers are
    // a stand-in for a real discovery, not the standard's answer.
    out->hans_number = count_distinct_modules(bm, HES_MODTYPE_HI);
    out->wans_number = count_distinct_modules(bm, HES_MODTYPE_WI);

    // 'ns' counts SERVICE MODULES (Table 42). Hosting a binding map is what
    // makes a module a service module (15045-4-1 A.4), and this one does -- but
    // it is the host of the table, not a row in it: the addressing table lists
    // the modules the gateway reaches, i.e. the HAN/WAN ones behind its
    // devices. So the 1 is this module itself, and the helper adds any 'sm'
    // rows the table declares for other service modules.
    out->service_modules_number = 1 + count_distinct_modules(bm, HES_MODTYPE_SM);

    // One row per binding map; this gateway has exactly one, here.
    if (out->n_binding_maps < ID_MAX_BINDING_MAPS) {
        id_binding_map_t* row = &out->binding_maps[out->n_binding_maps++];
        hes_strlcpy(row->module_type, sizeof(row->module_type), "sm");
        row->module_ref_index = 1;  // POC convention: service modules number from 1
        row->net_ref_index = 0;     // 0 = the module itself (Table 39)
    }
}

////////////////////////////////////////////////////////////////////////////////
/// The time service's view of the world, taken from the sync thread.
///
/// The time objects ask for this rather than holding the sync handle themselves:
/// it keeps services/time/time.c free of network code, and it lets a test supply
/// a fixed snapshot.
static void core_time_snapshot(void* ctx, time_snapshot_t* out)
{
    time_sync_get((time_sync_t*)ctx, out);
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

int core_main(const core_config_t* cfg)
{
    // Bound to local names: the launch settings are read all over this function.
    const char* pub_url = cfg->pub_url;
    const char* sub_url = cfg->sub_url;
    const char* bm_xml = cfg->bm_xml;
    const char* lua_app = cfg->lua_app;
    const char* auth_policy = cfg->auth_policy;
    const char* identity_path = cfg->identity_path;

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    log_trace("starting, pub(downstream)=%s sub(upstream)=%s\n", pub_url, sub_url);

    // --- identification service: first, and mandatory ---
    // 18012-3 11.2.2.1 requires exactly one, and its identity document holds
    // the gateway's secret fingerprint. A gateway that cannot present a
    // well-formed, stationary identity must not join the network, so this is
    // checked -- and fails closed -- before anything else starts. Note the
    // service deliberately never regenerates a missing ID: doing so would
    // break the 'ro' requirement that 'pi' be stationary.
    //
    // It contributes one object per Lexicon address it answers on
    // (/lx/ob/so/id, .../co/st/cv, /lx/ac/so/id/dc/bm/cv), all sharing one state.
    // One object per address because the router finds an object by matching
    // the message's path. The state behind them is a single instance, so the
    // three addresses are three doors onto the same identity: objs[0] is as
    // good a handle as any of them.
    service_object_t* objs[CORE_MAX_SERVICE_OBJECTS] = {0};
    int n_objs = id_service_create(identity_path, objs, CORE_MAX_SERVICE_OBJECTS);
    if (n_objs < 0) {
        log_error("no valid identity document; refusing to start");
        return 1;
    }
    service_object_t* id_so = objs[0];  // one state behind every id address

    hes_bus_t bus = {0};
    int rv = hes_bus_hub_open(&bus, pub_url, sub_url);
    if (rv != 0) {
        for (int i = 0; i < n_objs; i++) {
            objs[i]->destroy(objs[i]);
            free(objs[i]);
        }
        return 1;
    }

    // --- time service: the network-backed halves first ---
    // Figure 12's "time source & sync" and "time zone & daylight map": an SNTP
    // client and an HTTPS zone lookup, both on their own thread so that the bus
    // loop above never waits on somebody else's server. With no Internet they
    // simply never succeed and the service keeps reporting the host clock and
    // the 'lx' (local crystal) time source, so nothing here is fatal.
    time_sync_config_t sync_cfg = {0};
    sync_cfg.ntp_servers = cfg->ntp_servers;
    sync_cfg.ntp_enabled = cfg->ntp_enabled;
    sync_cfg.tz_name = cfg->tz_name;
    sync_cfg.tz_urls = cfg->tz_urls;
    sync_cfg.tz_enabled = cfg->tz_enabled;
    time_sync_t* time_sync = time_sync_start(&sync_cfg);

    // --- the remaining service objects ---
    // The time service answers on three addresses (11.2.3.2 realTime, 11.2.3.3
    // localTimeZone, 11.2.3.4 sourceOfTime), one object each, all sharing one
    // state. They read the sync thread's snapshot through this hook instead of
    // owning the thread, which also keeps the service testable offline.
    time_service_config_t time_cfg = {0};
    time_cfg.snapshot = core_time_snapshot;
    time_cfg.snapshot_ctx = time_sync;

    int time_objs =
            time_service_create(&time_cfg, objs + n_objs, CORE_MAX_SERVICE_OBJECTS - n_objs);
    if (time_objs < 0) {
        log_error("cannot create the time service; refusing to start");
        time_sync_stop(time_sync);
        for (int i = 0; i < n_objs; i++) {
            objs[i]->destroy(objs[i]);
            free(objs[i]);
        }
        hes_bus_close(&bus);
        return 1;
    }
    int time_realtime = n_objs + TIME_OBJECT_REALTIME;
    n_objs += time_objs;

    service_object_t* auth_so = auth_service_create(auth_policy);
    objs[n_objs++] = auth_so;

    int crypto_start = n_objs;
    int crypto_objs =
            crypto_service_create(objs + crypto_start, CORE_MAX_SERVICE_OBJECTS - crypto_start);
    if (crypto_objs < 0) {
        log_error("cannot create the cryptographic service; refusing to start");
        time_sync_stop(time_sync);
        for (int i = 0; i < n_objs; i++) {
            objs[i]->destroy(objs[i]);
            free(objs[i]);
        }
        hes_bus_close(&bus);
        return 1;
    }
    int crypto_ci = crypto_start + CRYPTO_OBJECT_CIPHERS;
    n_objs += crypto_objs;

    // --- identity key handover: 'fp' -> the one component that spends it ---
    // 11.2.2.1 says the digitalFingerprint is "used for encryption techniques",
    // and the only cryptographic operations the gateway performs with it are
    // the crypto service's. So the key derived from it moves there, once, here:
    // id_service_export_identity_key() wipes the identification service's own
    // copy as it hands the key over, so at most one copy of the secret exists
    // in the process at any moment.
    //
    // Fatal rather than a warning, deliberately: a gateway that cannot sign
    // would still look healthy while quietly offering no masquerade protection
    // at all, and that is worse than not starting.
    unsigned char identity_key[ID_IDENTITY_KEY_BYTES] = {0};
    rv = id_service_export_identity_key(id_so, identity_key);
    if (rv != 0) {
        log_error("no identity key to give the crypto service; refusing to start");
        time_sync_stop(time_sync);
        for (int i = 0; i < n_objs; i++) {
            objs[i]->destroy(objs[i]);
            free(objs[i]);
        }
        hes_bus_close(&bus);
        return 1;
    }
    crypto_service_set_identity_key(objs[crypto_ci], identity_key);

    // The local copies have served their purpose; the crypto service has one.
    memset(identity_key, 0, sizeof(identity_key));

    // --- binding map: the module's sole gateway onto HES-CLME ---
    binding_map_t bm = {0};
    bm_init(&bm);
    rv = bm_load_xml(&bm, bm_xml);
    if (rv != 0) {
        log_warn("continuing with an empty binding map");
    }

    // Development switch --no-authz: take authorization out of the picture
    // entirely -- whatever this XML declares and whatever the policy below
    // writes. Done before the propagation so the ordering is obvious to read,
    // though it would not matter: the gates ignore the 'at' fields either way.
    if (cfg->no_authz) {
        bm_disable_authorization(&bm);
        log_warn("authorization DISABLED (--no-authz): 'at' and 'ap' gates are ignored");
    }

    // --- authorization: apply the read-only policy to the binding map ---
    // For every class, the A&A service writes each applicable permission into
    // the binding map's authorType ('at') fields. From then on the binding map
    // enforces those fields locally, with no per-message work -- or, under
    // --no-authz, the writes land in rows whose gates no longer consult them.
    //
    // Same-process shortcut: NNG never delivers a message back to the process
    // that sent it, so this is a direct call rather than a self-addressed PUT.
    // A binding map living in another module would receive the same address as
    // a real PUT over the bus.
    int auth_writes = auth_service_propagate(auth_so, core_write_at, &bm);
    log_trace("authorization propagated %d permission write(s)", auth_writes);

    // --- identification: where the inventory answers come from ---
    // The id service cannot enumerate the gateway's modules itself. That list
    // comes from the manifest service (the product profile is this box's own
    // inventory of modules); the addressing table is only the fallback, for when
    // there is no manifest service to ask. Hand the hook over, then refresh the
    // list once here and periodically in the loop below.
    core_inventory_t inventory = {0};
    inventory.bm = &bm;
    inventory.svc_rep_url = cfg->manifest_rep_url;
    core_inventory_refresh(&inventory);
    id_service_set_inventory_source(id_so, core_gateway_inventory, &inventory);

    // bm_controller_start runs once at startup (from service_modules/core/main.c).
    // After that, bm_processor_handle runs continuously: when an EVENT/GET
    // response arrives whose path matches an operation's source_object, it
    // stores the value in value_cache[device_index] and calls
    // bm_evaluate_all(). An operation only fires once all its inputs are valid
    // (op_ready()), which is why the controller + processor form one coherent
    // dataflow:
    //
    // - external values -> subscribed events -> value_cache -> operations fire
    //   -> results cached at out_device_index -> which can unlock downstream
    //   'it'-chained operations (via the recursive bm_evaluate_all) -> PUT to
    //   the destination object on the bus.
    //
    // In one sentence: the controller sets up the data sources the gateway
    // depends on, and the processor consumes them to bind each source to its
    // destination and convert the value. That matches the standard's split of
    // "controller for setup/configuration, processor for real-time operation".
    bm_controller_start(&bm, &bus);

    // --- customer-specific protected app (Lua) ---
    app_lua_t* app = app_lua_create(lua_app, &bus, objs, n_objs);

    // Let the app take part in the binding map's dataflow: a row whose
    // 'op' is "ap" (appService) has its operation performed by the script
    // instead of by the built-in operation set.
    //
    // Optional -- without it the map stays pure XML. The app is reached
    // only this way: moduleType 'it' keeps its meaning of "internal
    // process" for ordinary chaining, so it is not used as an app address.
    // See src/bm/README.md, "Routing data through the Lua app".
    bm.app_ctx = app;
    bm.app_operation = app_service_op;

    // Mode B: per-message authorization for rows whose inputs say ap='au'.
    // 'at' (above) is pre-computed and cannot vary per sender; this hook covers
    // the case where several senders share a row, by checking the credential
    // that travels with each message. See src/services/auth/README.md.
    //
    // The binding map asks, this pairing answers -- policy first, then the proof
    // gate if the policy demands one. Lives as long as the binding map, which is
    // to say for the whole run.
    core_authorizer_t authorizer = {auth_so, objs[crypto_ci]};
    bm.auth_ctx = &authorizer;
    bm.auth_authorize = core_authorize;

    fprintf(stderr, "hes-core-module: running. Ctrl-C to stop.\n");

    time_t last_internal_feed = 0;
    time_t last_inventory_refresh = 0;

    while (g_running) {
        hes_clme_msg_t in;
        // Block for at most 200 ms (timeout_ms).
        int rv = hes_bus_recv(&bus, &in, 200);
        if (rv == 0) {
            log_trace("recv verb=%u path=%s payload=%s", in.verb, in.path, in.payload);

            int handled = dispatch_to_local_objects(&bus, objs, n_objs, &in);

            // Relay an external module's PUT to a declared device
            // destination, downstream to the interface module that fronts the
            // device. Guarded so the core does not become a blind proxy: only
            // addressing-table rows whose module type is 'hi'/'wi' (a real
            // device) are accepted; anything else is dropped, preserving the
            // "leaf modules cannot reach each other" property documented in
            // common/hes_bus.h.
            if (!handled && in.verb == HES_VERB_PUT &&
                bm_is_declared_destination(&bm, in.path, in.device_index)) {
                log_trace("relaying PUT %s di=%u = %s downstream", in.path, in.device_index,
                          in.payload);
                hes_bus_send(&bus, &in);
            }

            // Hand the message to the binding map's processor, which lifts the
            // value out of an EVENT report (or a GET answer that carries one)
            // and ignores everything else -- a PUT is a command, not a report,
            // so it never reaches the value cache. Unconditional on purpose:
            // one funnel, with "which verbs carry a value" decided next to the
            // cache they feed (see bm_processor_handle()).
            bm_processor_handle(&bm, &bus, &in);
        }
        // NNG_ETIMEDOUT is expected every 200ms when the bus is idle -- not an
        // error, just means it's time to run the periodic work below.

        // Periodic (tick) work for local service objects, e.g. the time
        // service's autonomous clock.
        for (int i = 0; i < n_objs; i++) {
            if (objs[i]->tick) {
                objs[i]->tick(objs[i], &bus);
            }
        }

        // Feed the time service's value into our own binding map once per
        // second WITHOUT going over the bus.
        //
        // Why: the time service object and the binding map both live in this
        // one process. NNG sockets never deliver a message back to the very
        // process that sent it, so if the time service "sent" its event out
        // of our socket, the binding map sitting beside it here would never
        // receive it. Instead we synthesize the event message in memory and
        // hand it straight to the binding map's processor -- a plain function
        // call instead of a wire.
        //
        // ISO/IEC 18012-3 Annex A.1 allows this: how objects reach their own
        // module's binding map is implementation-defined ("directly or through
        // service objects", not necessarily over the bus).
        //
        // The answer carries every table of the realTime object as
        // "transCode=value" records; the binding map picks out the 'va' datum
        // (the current value) with bm_datum_value(). */
        time_t now = time(NULL);
        if (now != last_internal_feed) {
            last_internal_feed = now;
            service_object_t* time_rt = objs[time_realtime];
            hes_clme_msg_t self_event = {0};
            self_event.verb = HES_VERB_EVENT;
            hes_msg_set_path(&self_event, time_rt->path);
            time_rt->on_get(time_rt, &self_event);
            bm_processor_handle(&bm, &bus, &self_event);
        }

        app_lua_tick(app);

        // Re-read the gateway's module list now and then, so a module that came
        // up late is counted without restarting the core.
        if (now - last_inventory_refresh >= CORE_INVENTORY_REFRESH_S) {
            last_inventory_refresh = now;
            core_inventory_refresh(&inventory);
        }
    }

    log_trace("shutting down");
    app_lua_destroy(app);
    // Objects first: they read the sync snapshot, so the sync thread must
    // outlive them.
    for (int i = 0; i < n_objs; i++) {
        objs[i]->destroy(objs[i]);
        free(objs[i]);
    }

    time_sync_stop(time_sync);
    if (inventory.mreg_open) {
        hes_mreg_close(&inventory.mreg);
    }
    hes_bus_close(&bus);
    return 0;
}
