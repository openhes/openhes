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
/// @brief NNG client handle to the Device Manifest service: an interface module's
/// query/report channel plus its notification stream, and the gateway's module
/// list for the components that need to count modules.
///
/// @details
/// An interface module opens one of these instead of reading a
/// dev_manifest.json file. The module can then:
///   hes_mreg_query()         -- get MY device slice (only the devices this
///                               module manages), once at boot;
///   hes_mreg_report()        -- tell the service my real device info and
///                               presence (address, online/offline);
///   hes_mreg_recv_update()   -- poll for service notifications.
///
/// A component that only asks questions about the *gateway* (the identification
/// service's nh/nw/ns counts, Table 42) opens a registry-only handle instead --
/// hes_mreg_open_registry() -- and calls:
///   hes_mreg_query_modules() -- the modules this gateway declares, with their
///                               type and presence. This is the list the
///                               counts come from: it is the product profile's
///                               own inventory of the box, so no binding-map XML
///                               is involved (an integral gateway contains its
///                               modules; a modular one would discover them).
///
/// Access control: the service publishes notifications on topic =
/// moduleRefIndex, and answers a request only for the requested
/// (moduleType, moduleRefIndex) -- the pair is the module's identity, and a
/// profile regularly gives a service module ('sm') and an interface module
/// ('hi') the same index. A module SUBscribes only to its own topic, so at the
/// transport level it can only ever receive information about the devices
/// it manages (the same "per-module slice" principle that used to be a
/// file, docs/poc_design.md section 9.4).
///
/// Memory/ownership: the two sockets belong to the hes_mreg_t the caller supplies;
/// a slice is copied into the caller's hes_devreg_t.
///
/// Threading: no locking -- driven from the module's own loop, which polls
/// hes_mreg_recv_update() with a zero timeout.
///
#ifndef OPENHES_SRC_COMMON_HES_MREG_H
#define OPENHES_SRC_COMMON_HES_MREG_H

#include "hes_devreg.h"

#include <nng/nng.h>

////////////////////////////////////////////////////////////////////////////////
/// Client handle to the manifest service: what one module needs in order to
/// ask, report and be notified.
typedef struct hes_mreg {
    nng_socket req;             ///< -> service REP (query/report)
    nng_socket sub;             ///< -> service PUB (notifications)
    char module_type[8];        ///< this module's 'mt' (e.g. "hi"); half its identity
    uint32_t module_ref_index;  ///< this module's 'mi'
    char topic[16];             ///< decimal moduleRefIndex; the SUB prefix
} hes_mreg_t;

/// Room for the modules of one gateway, i.e. of its product profile.
#define HES_MODULES_MAX 16

////////////////////////////////////////////////////////////////////////////////
/// One module of the gateway, as the manifest service knows it: the fields
/// Table 39/42 count over.
typedef struct hes_module_entry {
    char module_type[8];          ///< 'mt': hi (HAN iface) | wi (WAN iface) | sm (service module)
    uint32_t module_ref_index;    ///< 'mi': the module's reference id
    char service_module_type[8];  ///< 'st': the service domain, for an 'sm' module (e.g. "id")
    int present;                  ///< 1 = declared and reported in, 0 = not seen yet
} hes_module_entry_t;

////////////////////////////////////////////////////////////////////////////////
/// The gateway's module list: what the profile declares, with presence.
typedef struct hes_modules {
    int n;  ///< how many entries modules[] holds
    hes_module_entry_t modules[HES_MODULES_MAX];
} hes_modules_t;

////////////////////////////////////////////////////////////////////////////////
/// Connects to the devicemanifest service: REQ to svc_rep_url (queries/reports)
/// and SUB to svc_pub_url, subscribed to our moduleRefIndex topic.
///
/// @param m The device manifest service client handle to initialize.
/// @param svc_rep_url The service's REP endpoint URL (e.g. HES_MANIFEST_REP_URL)
/// @param svc_pub_url The service's PUB endpoint URL (e.g. HES_MANIFEST_PUB_URL)
/// @param module_type The module's 'mt', e.g. HES_MT_INTERFACE for a HAN-facing
///                    interface module (NULL/"" asks by index alone)
/// @param module_ref_index The module's reference index
/// @return 0 on success, -1 on failure
int hes_mreg_open(hes_mreg_t* m,
                  const char* svc_rep_url,
                  const char* svc_pub_url,
                  const char* module_type,
                  uint32_t module_ref_index);

////////////////////////////////////////////////////////////////////////////////
/// Opens a registry-only client: REQ to svc_rep_url and no notification socket.
///
/// For a caller that only asks questions about the gateway (the module list
/// above) rather than receiving its own device slice. Unlike hes_mreg_open()
/// this dials once and gives up: discovery is optional, so a service that is not
/// up yet must not stall the caller's loop -- call it again later to retry.
/// hes_mreg_recv_update() is not usable on such a handle; hes_mreg_close() is.
///
/// @param m The client handle to initialize.
/// @param svc_rep_url The service's REP endpoint URL.
/// @param timeout_ms Reply timeout, or 0 for the default.
/// @return 0 on success, -1 when the service could not be reached.
int hes_mreg_open_registry(hes_mreg_t* m, const char* svc_rep_url, int timeout_ms);

////////////////////////////////////////////////////////////////////////////////
/// Asks the service for this module's device slice and parse it into *reg.
///
/// @param m The device manifest service client handle
/// @param reg The device registry to populate with the module's device slice
/// @return 0 on success, -1 on failure
int hes_mreg_query(hes_mreg_t* m, hes_devreg_t* reg);

////////////////////////////////////////////////////////////////////////////////
/// Asks the service for the gateway's module list (every module the product
/// profile declares, with its type and presence).
///
/// @param m A handle from hes_mreg_open_registry() (or hes_mreg_open()).
/// @param out Receives the module list.
/// @return 0 on success, -1 on failure (the caller should keep its last answer).
int hes_mreg_query_modules(hes_mreg_t* m, hes_modules_t* out);

////////////////////////////////////////////////////////////////////////////////
/// Reports real device info / presence to the service (which updates its
/// in-memory registry and notifies subscribers).
///
/// @param m The device manifest service client handle
/// @param reg The device registry containing the device information to report
/// @return 0 on success, -1 on failure
int hes_mreg_report(hes_mreg_t* m, const hes_devreg_t* reg);

////////////////////////////////////////////////////////////////////////////////
/// Poll for a service notification (a fresh slice for this module).
///
/// @param m The device manifest service client handle
/// @param reg The device registry to populate with the updated device slice
/// @param timeout_ms The timeout in milliseconds
/// @return 0 when a slice arrived (parsed into *reg), otherwise -1 -- see the
///         NNG error (e.g. NNG_ETIMEDOUT for "nothing waiting").
int hes_mreg_recv_update(hes_mreg_t* m, hes_devreg_t* reg, int timeout_ms);

////////////////////////////////////////////////////////////////////////////////
/// Closes the device manifest service client handle.
///
/// @param m The device manifest service client handle to close
void hes_mreg_close(hes_mreg_t* m);

#endif  // #ifndef OPENHES_SRC_COMMON_HES_MREG_H
