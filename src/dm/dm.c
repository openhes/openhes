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
/// @brief The Device Manifest service: a "system" component (NOT part of ISO/IEC
/// 15045/18012 -- see docs/poc_design.md §9.3-9.4, §12).
///
/// @details
/// The ONLY file is the static Product Profile Manifest (virtual identities:
/// deviceIndex/role/hardware/objects, no real addresses). Everything else lives
/// in an IN-MEMORY registry served over NNG:
///
///   query  (REQ/REP)  -- a module asks for ITS device slice; the reply
///                        contains only the devices of that moduleRefIndex
///   modules(REQ/REP)  -- the gateway's module list, i.e. every module the
///                        product profile declares (its type and presence).
///                        This is the one question a module cannot ask about
///                        itself, and it is what the identification service's
///                        nh/nw/ns counts are derived from (Table 39/42)
///   report (REQ/REP)  -- an interface module tells the service about a
///                        REAL device it manages (address, udpPort,
///                        online/offline); the service stores it
///   notify (PUB/SUB)  -- whenever a device's info changes the service
///                        publishes the updated slice on topic =
///                        moduleRefIndex, so a module only receives its
///                        OWN devices ("a HAN only gets the devices it
///                        manages", enforced at the transport level)
///
/// Interface modules report real presence because they are the ones that
/// actually see the network (e.g. a WiZ bulb that stops answering getPilot).
///
/// Memory/ownership: the registry is in-memory and file-static, sized by
/// MS_MAX_MODULES/MS_MAX_DEVICES. Only the Product Profile Manifest is read from
/// disk, and its JSON tree is released once loaded.
///
/// Threading: single-threaded -- one loop serves the REQ/REP and PUB sockets;
/// a signal only sets the stop flag.

#include "dm.h"

#include "app_config.h"
#include "utils.h"

#include <argtable3.h>
#include <jansson.h>
#include <log.h>
#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/reqrep0/rep.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MS_MAX_MODULES 8
#define MS_MAX_DEVICES 16
#define MS_MAX_OBJECTS 8
#define MS_PATH 192
#define MS_MAX_REQ_BUF_SIZE 16384

typedef struct ms_object {
    char path[MS_PATH];  ///< lexicon object path, e.g. /lx/ob/uo/li/ll/da/cv
    char access[8];      ///< access mode: "r" | "w" | "rw"
} ms_object_t;

typedef struct ms_devprofile {
    long device_index;                    ///< 'di' -- gateway-wide, matches addressingTable
    long net_ref_index;                   ///< 'ni' -- network-wide, matches networkTable
    char role[64];                        ///< device role
    char expected_hardware[96];           ///< expected hardware
    char network_type[16];                ///< network type
    int n_objects;                        ///< number of objects
    ms_object_t objects[MS_MAX_OBJECTS];  ///< array of objects

    /// Runtime state -- fed by interface module "report" messages, NOT
    /// by the product profile file.
    char runtime_address[MS_PATH];  ///< real address (MAC/IP) reported by the module
    int runtime_udp_port;           ///< real UDP port reported by the module
    int runtime_bound;              ///< a module has reported a real device for this di
    int online;                     ///< last reported presence
} ms_devprofile_t;

typedef struct ms_module {
    char module_type[8];                   ///< module type
    long module_ref_index;                 ///< module reference index
    char service_module_type[8];           ///< service domain, for a service module (e.g. "id")
    char network_type[16];                 ///< network type
    int n_devs;                            ///< number of devices
    ms_devprofile_t devs[MS_MAX_DEVICES];  ///< array of device profiles
} ms_module_t;

typedef struct ms_profile {
    int n_modules;                        ///< number of modules
    ms_module_t modules[MS_MAX_MODULES];  ///< array of modules
} ms_profile_t;

/// One persisted binding (real address bound to a logical device).
typedef struct ms_binding {
    long module_ref_index;  ///< module reference index
    long device_index;      ///< device index
    long net_ref_index;     ///< network reference index
    char network_type[16];  ///< network type
    char mac[64];           ///< MAC address
    char ip[64];            ///< IP address
    int udp_port;           ///< UDP port
} ms_binding_t;

#define MS_MAX_BINDINGS (MS_MAX_MODULES * MS_MAX_DEVICES)
typedef struct ms_bindings {
    int n;                            ///< number of bindings
    ms_binding_t b[MS_MAX_BINDINGS];  ///< array of bindings
} ms_bindings_t;

/// What a discovery/adapter reports as present right now.
typedef struct ms_seen {
    int n;                      ///< number of seen devices
    char network_type[16][16];  ///< array of network types
    char mac[16][64];           ///< array of MAC addresses
} ms_seen_t;

static volatile sig_atomic_t g_running = 1;

////////////////////////////////////////////////////////////////////////////////
/// SIGINT/SIGTERM handler: ask the service loop to finish its current pass.
///
/// @param sig The signal number (unused).
static void on_sigint(int sig)
{
    (void)sig;
    g_running = 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Reads a numeric member of a JSON object.
///
/// @param o The JSON object to read from.
/// @param key The member name.
/// @param def Value returned when the member is missing or not a number.
/// @return The member's value, or def.
static long obj_long(const json_t* o, const char* key, long def)
{
    const json_t* m = json_object_get(o, key);
    if (json_is_number(m)) {
        return (long)json_number_value(m);
    }
    return def;
}

////////////////////////////////////////////////////////////////////////////////
/// Copies a string member of a JSON object into a fixed-size buffer.
///
/// @param o The JSON object to read from.
/// @param key The member name.
/// @param dst Destination buffer.
/// @param dstsz Capacity of dst.
static void obj_str(const json_t* o, const char* key, char* dst, size_t dstsz)
{
    const json_t* m = json_object_get(o, key);
    if (json_is_string(m)) {
        snprintf(dst, dstsz, "%s", json_string_value(m));
    }
}

////////////////////////////////////////////////////////////////////////////////
/// Loads a product profile from a file to internal structure, which serves as
/// the source of truth for the device manager.
///
/// @param pf The profile to load into.
/// @param path The path to the file to read.
/// @return 0 on success, -1 on failure.
static int load_profile(ms_profile_t* pf, const char* path)
{
    memset(pf, 0, sizeof(*pf));
    char* txt = read_file(path);
    if (!txt) {
        log_error("cannot read profile '%s'", path);
        return -1;
    }

    json_error_t error;
    json_t* root = json_loads(txt, 0, &error);
    if (!root) {
        log_error("cannot parse profile '%s': %s", path, error.text);
        free(txt);
        return -1;
    }
    free(txt);

    const json_t* modules = json_object_get(root, "modules");
    if (!json_is_array(modules)) {
        log_error("profile has no \"modules\" array");
        json_decref(root);
        return -1;
    }

    size_t nm = json_array_size(modules);
    for (int i = 0; i < nm && pf->n_modules < MS_MAX_MODULES; i++) {
        const json_t* mo = json_array_get(modules, i);
        if (!json_is_object(mo)) {
            continue;
        }

        ms_module_t* mod = &pf->modules[pf->n_modules];
        obj_str(mo, "moduleType", mod->module_type, sizeof(mod->module_type));
        obj_str(mo, "networkType", mod->network_type, sizeof(mod->network_type));
        obj_str(mo, "serviceModuleType", mod->service_module_type,
                sizeof(mod->service_module_type));
        mod->module_ref_index = obj_long(mo, "moduleRefIndex", 0);

        const json_t* dps = json_object_get(mo, "deviceProfiles");
        if (json_is_array(dps)) {
            size_t nd = json_array_size(dps);
            for (size_t d = 0; d < nd && mod->n_devs < MS_MAX_DEVICES; d++) {
                const json_t* dp = json_array_get(dps, d);
                if (!json_is_object(dp)) {
                    continue;
                }

                ms_devprofile_t* dev = &mod->devs[mod->n_devs];
                dev->device_index = obj_long(dp, "deviceIndex", 0);
                dev->net_ref_index = obj_long(dp, "netRefIndex", 0);
                obj_str(dp, "deviceRole", dev->role, sizeof(dev->role));
                obj_str(dp, "expectedHardware", dev->expected_hardware,
                        sizeof(dev->expected_hardware));
                obj_str(dp, "networkType", dev->network_type, sizeof(dev->network_type));

                const json_t* objs = json_object_get(dp, "objects");
                if (json_is_array(objs)) {
                    size_t no = json_array_size(objs);
                    for (size_t o = 0; o < no && dev->n_objects < MS_MAX_OBJECTS; o++) {
                        const json_t* ob = json_array_get(objs, o);
                        if (!json_is_object(ob)) {
                            continue;
                        }
                        obj_str(ob, "lexiconPath", dev->objects[dev->n_objects].path, MS_PATH);
                        obj_str(ob, "access", dev->objects[dev->n_objects].access,
                                sizeof(dev->objects[dev->n_objects].access));
                        dev->n_objects++;
                    }
                }
                mod->n_devs++;
            }
        }
        pf->n_modules++;
    }
    json_decref(root);
    if (pf->n_modules == 0) {
        log_error("no modules in profile");
        return -1;
    }
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Finds the module with the given (moduleType, moduleRefIndex).
///
/// The pair is the module's identity: a profile regularly holds a service module
/// ('sm') and an interface module ('hi') that carry the same moduleRefIndex, so a
/// lookup by index alone hands out whoever comes first in the file -- typically
/// the identification service's slice, which has no devices. An empty (or NULL)
/// module_type keeps the index-only behaviour for a client that does not say.
///
/// @param pf The product profile.
/// @param module_type The 'mt' to match, or NULL/"" for any.
/// @param mi The moduleRefIndex to look for.
/// @return A pointer to the module, or NULL when the profile declares none.
static ms_module_t* find_module(ms_profile_t* pf, const char* module_type, long mi)
{
    int want_type = (module_type != NULL && module_type[0] != '\0');
    for (int i = 0; i < pf->n_modules; i++) {
        ms_module_t* mod = &pf->modules[i];
        if (mod->module_ref_index != mi) {
            continue;
        }
        if (want_type && strcmp(mod->module_type, module_type) != 0) {
            continue;
        }

        return mod;
    }

    return NULL;
}

////////////////////////////////////////////////////////////////////////////////
/// Finds a device (by deviceIndex) inside a module.
///
/// @param mod The module to search.
/// @param di The device index to find.
/// @return A pointer to the device if found, otherwise NULL.
static ms_devprofile_t* find_device(ms_module_t* mod, long di)
{
    for (int i = 0; i < mod->n_devs; i++) {
        if (mod->devs[i].device_index == di) {
            return &mod->devs[i];
        }
    }

    return NULL;
}

////////////////////////////////////////////////////////////////////////////////
/// Renders one module's device slice into a dynamic NUL-terminated JSON string.
///
/// @param mod The module to render.
/// @return A dynamic string owned by the caller, or NULL on allocation failure.
static arg_dstr_t build_slice(const ms_module_t* mod)
{
    arg_dstr_t slice = arg_dstr_create();
    if (!slice) {
        return NULL;
    }

    arg_dstr_catf(slice, "{\n");
    arg_dstr_catf(slice, "  \"gatewayId\": \"HES-GW-2026-99A1\",\n");
    arg_dstr_catf(slice, "  \"moduleType\": \"%s\",\n", mod->module_type);
    arg_dstr_catf(slice, "  \"moduleRefIndex\": %ld,\n", mod->module_ref_index);
    arg_dstr_catf(slice, "  \"networkType\": \"%s\",\n", mod->network_type);
    arg_dstr_catf(slice, "  \"devices\": [\n");
    for (int d = 0; d < mod->n_devs; d++) {
        const ms_devprofile_t* dev = &mod->devs[d];
        const char* status = dev->runtime_bound ? (dev->online ? "online" : "offline") : "unbound";
        const char* nt = dev->network_type[0] ? dev->network_type : mod->network_type;
        arg_dstr_catf(slice, "%s    {\n", d ? ",\n" : "");
        arg_dstr_catf(slice, "      \"deviceIndex\": %ld,\n", dev->device_index);
        arg_dstr_catf(slice, "      \"netRefIndex\": %ld,\n", dev->net_ref_index);
        arg_dstr_catf(slice, "      \"deviceRole\": \"%s\",\n", dev->role);
        arg_dstr_catf(slice, "      \"expectedHardware\": \"%s\",\n", dev->expected_hardware);
        arg_dstr_catf(slice, "      \"networkType\": \"%s\",\n", nt);
        arg_dstr_catf(slice, "      \"address\": \"%s\",\n", dev->runtime_address);
        arg_dstr_catf(slice, "      \"udpPort\": %d,\n", dev->runtime_udp_port);
        arg_dstr_catf(slice, "      \"status\": \"%s\",\n", status);
        arg_dstr_catf(slice, "      \"objects\": [\n");
        for (int o = 0; o < dev->n_objects; o++) {
            arg_dstr_catf(slice, "        { \"lexiconPath\": \"%s\", \"access\": \"%s\" }%s\n",
                          dev->objects[o].path, dev->objects[o].access,
                          (o + 1 < dev->n_objects) ? "," : "");
        }
        arg_dstr_catf(slice, "      ]\n    }");
    }
    arg_dstr_catf(slice, "\n  ]\n}\n");
    return slice;
}

////////////////////////////////////////////////////////////////////////////////
/// Sends a reply back on the REQ/REP socket.
///
/// @param rep The REQ/REP socket.
/// @param s The string to send.
static void reply_text(nng_socket rep, const char* s)
{
    nng_send(rep, (void*)s, strlen(s), 0);
}

////////////////////////////////////////////////////////////////////////////////
/// Has this module reported in?
///
/// An interface module answers for its devices: present once it has reported a
/// real address for at least one of them. A service module has no devices of
/// its own, and this POC has no service-module presence report yet, so one that
/// the profile declares counts as present -- in an integral gateway it is inside
/// this same box, and the box is answering.
///
/// @param mod The module to inspect.
/// @return 1 when the module counts as present, 0 when it has not reported yet.
static int module_present(const ms_module_t* mod)
{
    for (int i = 0; i < mod->n_devs; i++) {
        if (mod->devs[i].runtime_bound) {
            return 1;
        }
    }
    return mod->n_devs == 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Renders the gateway's module list into a dynamic NUL-terminated JSON string:
/// one entry per module the product profile declares, with its type, its
/// reference index, the service domain for a service module, and whether it has
/// reported in. This is what the identification service counts to fill Table
/// 42's nh/nw/ns -- "the HES gateway system", not one module's own view.
///
/// @param pf The product profile.
/// @return A dynamic string owned by the caller, or NULL on allocation failure.
static arg_dstr_t build_modules_list(const ms_profile_t* pf)
{
    arg_dstr_t list = arg_dstr_create();
    if (!list) {
        return NULL;
    }

    arg_dstr_catf(list, "{\n  \"modules\": [\n");
    for (int i = 0; i < pf->n_modules; i++) {
        const ms_module_t* mod = &pf->modules[i];
        arg_dstr_catf(list, "%s    {\n", i ? ",\n" : "");
        arg_dstr_catf(list, "      \"moduleType\": \"%s\",\n", mod->module_type);
        arg_dstr_catf(list, "      \"moduleRefIndex\": %ld,\n", mod->module_ref_index);
        arg_dstr_catf(list, "      \"serviceModuleType\": \"%s\",\n", mod->service_module_type);
        arg_dstr_catf(list, "      \"present\": %d\n", module_present(mod));
        arg_dstr_catf(list, "    }");
    }
    arg_dstr_catf(list, "\n  ]\n}\n");
    return list;
}

////////////////////////////////////////////////////////////////////////////////
/// Publishes a fresh slice for a module on the PUB socket. The message is
/// "<moduleRefIndex>" + slice, so subscribers (which filter by that
/// prefix) only receive their own module's devices.
///
/// @param pub The PUB socket.
/// @param mod The module for which to publish a slice.
static void publish_slice(nng_socket pub, const ms_module_t* mod)
{
    arg_dstr_t slice = build_slice(mod);
    if (!slice) {
        return;
    }

    arg_dstr_t msg = arg_dstr_create();
    if (msg) {
        arg_dstr_catf(msg, "%ld%s", mod->module_ref_index, arg_dstr_cstr(slice));
        nng_send(pub, (void*)arg_dstr_cstr(msg), strlen(arg_dstr_cstr(msg)), 0);
        arg_dstr_destroy(msg);
    }
    arg_dstr_destroy(slice);
}

////////////////////////////////////////////////////////////////////////////////
/// Handles one query/report request.
///
/// Examples:
///   {"cmd": "query", "moduleRefIndex": 1}
///   {"cmd": "report", "moduleRefIndex": 1, "devices": [
///       {"deviceIndex":6,
///        "address":"54:6C:0E:B7:20:04",
///        "udpPort":0,
///        "online":1
///       }
///     ]
///   }
///
/// @param pf The product profile.
/// @param rep The REQ/REP socket for sending replies.
/// @param pub The PUB socket for publishing notifications.
/// @param req The request string received from the client.
static void handle_request(ms_profile_t* pf, nng_socket rep, nng_socket pub, const char* req)
{
    json_error_t error;
    json_t* root = json_loads(req, 0, &error);
    if (!root) {
        reply_text(rep, "{\"error\":\"bad json\"}");
        return;
    }

    char cmd[MAX_CMD_SIZE] = {0};
    obj_str(root, "cmd", cmd, sizeof(cmd));

    // "modules" names no module: it answers "what is in this gateway?", which
    // is the one question a module cannot ask about itself.
    if (strcmp(cmd, "modules") == 0) {
        arg_dstr_t list = build_modules_list(pf);
        if (list) {
            reply_text(rep, arg_dstr_cstr(list));
            arg_dstr_destroy(list);
        } else {
            reply_text(rep, "{\"error\":\"out of memory\"}");
        }
        json_decref(root);
        return;
    }

    long mi = obj_long(root, "moduleRefIndex", -1);
    if (mi < 0) {
        json_decref(root);
        reply_text(rep, "{\"error\":\"missing moduleRefIndex\"}");
        return;
    }

    // Which module this is, when the client says so: (moduleType, moduleRefIndex)
    // is the identity, and it is what tells the identification service module
    // apart from the interface module that shares its index.
    char mt[MAX_CMD_SIZE] = {0};
    obj_str(root, "moduleType", mt, sizeof(mt));

    ms_module_t* mod = find_module(pf, mt, mi);
    if (!mod) {
        json_decref(root);
        char err[MAX_ERR_MSG_SIZE] = {0};
        if (mt[0] != '\0') {
            snprintf(err, sizeof(err), "{\"error\":\"no module %s/%ld\"}", mt, mi);
        } else {
            snprintf(err, sizeof(err), "{\"error\":\"no module %ld\"}", mi);
        }
        reply_text(rep, err);
        return;
    }

    if (strcmp(cmd, "query") == 0) {
        arg_dstr_t slice = build_slice(mod);
        if (slice) {
            reply_text(rep, arg_dstr_cstr(slice));
            arg_dstr_destroy(slice);
        } else {
            reply_text(rep, "{\"error\":\"out of memory\"}");
        }
        json_decref(root);
        return;
    }

    if (strcmp(cmd, "report") == 0) {
        // A module reports the real devices it currently manages.
        const json_t* devs = json_object_get(root, "devices");
        int changed = 0;
        if (json_is_array(devs)) {
            size_t n = json_array_size(devs);
            for (size_t i = 0; i < n; i++) {
                const json_t* e = json_array_get(devs, i);
                if (!json_is_object(e)) {
                    continue;
                }

                // Match the reported physical device to its virtual profile entry.
                long di = obj_long(e, "deviceIndex", -1);
                ms_devprofile_t* dev = find_device(mod, di);
                if (!dev) {
                    continue;
                }

                // Address, port, and presence are runtime facts supplied by the module.
                char addr[MS_PATH] = {0};
                obj_str(e, "address", addr, sizeof(addr));
                long udp = obj_long(e, "udpPort", -1);
                long online = obj_long(e, "online", -1);

                // Update only fields that were included in the report. An address or
                // port may be omitted when the module is reporting a state change.
                dev->runtime_bound = 1;
                if (addr[0]) {
                    snprintf(dev->runtime_address, sizeof(dev->runtime_address), "%s", addr);
                }

                if (udp >= 0) {
                    dev->runtime_udp_port = (int)udp;
                }

                if (online >= 0) {
                    dev->online = (int)(online != 0);
                }

                changed = 1;

                // Keep the service log useful when diagnosing module/device bindings.
                log_info("module %ld device %ld -> addr=%s udp=%d %s\n", mi, di,
                         dev->runtime_address, dev->runtime_udp_port,
                         dev->online ? "online" : "offline");
            }
        }

        // Publish the module's refreshed device slice, then acknowledge the report.
        if (changed) {
            publish_slice(pub, mod);
        }

        reply_text(rep, "{\"ok\":1}");
        json_decref(root);
        return;
    }

    json_decref(root);
    reply_text(rep, "{\"error\":\"unknown cmd\"}");
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

int manifest_service_main(const char* profile, const char* rep_url, const char* pub_url)
{
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    ms_profile_t pf = {0};
    int rv = load_profile(&pf, profile);
    if (rv != 0) {
        log_error("cannot load profile '%s'", profile);
        return 1;
    }

    nng_socket rep = {0};
    rv = nng_rep0_open(&rep);
    if (rv != 0) {
        log_error("failed to open REP socket");
        return 1;
    }

    rv = nng_socket_set_ms(rep, NNG_OPT_RECVTIMEO, 200);
    if (rv != 0) {
        log_error("failed to set receive timeout");
        nng_close(rep);
        return 1;
    }

    rv = nng_listen(rep, rep_url, NULL, 0);
    if (rv != 0) {
        log_error("failed to listen on %s for query and report", rep_url);
        nng_close(rep);
        return 1;
    }

    nng_socket pub = {0};
    rv = nng_pub0_open(&pub);
    if (rv != 0) {
        log_error("failed to open PUB socket");
        nng_close(rep);
        return 1;
    }

    rv = nng_listen(pub, pub_url, NULL, 0);
    if (rv != 0) {
        log_error("failed to listen on %s for notifications", pub_url);
        nng_close(pub);
        nng_close(rep);
        return 1;
    }

    log_info(
            "in-memory registry up. "
            "query/report @ %s, notify @ %s. Ctrl-C to stop.",
            rep_url, pub_url);

    while (g_running) {
        char req[MS_MAX_REQ_BUF_SIZE] = {0};
        size_t sz = sizeof(req) - 1;
        int rv = nng_recv(rep, req, &sz, 0);
        if (rv != 0) {
            continue;  // NNG_ETIMEDOUT: idle, loop again
        }

        req[sz] = '\0';
        handle_request(&pf, rep, pub, req);
    }

    nng_close(pub);
    nng_close(rep);
    log_info("stopping device manifest service");
    return 0;
}
