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
/// @brief Internal HES-CLME event bus: NNG pub/sub in a hub-and-leaf shape.
///
/// @details
/// Built on NNG's pub/sub protocol instead of bus0 -- deliberately, for the
/// isolation property bus0 doesn't give you: a HAN/WAN
/// interface module should never even receive traffic addressed to objects it
/// doesn't own, not just "receive it and ignore it." NNG's pub/sub subscription
/// filter (a raw byte-prefix match against the Lexicon path -- see hes_common.h
/// for why 'path' is the struct's first field) enforces that at the transport
/// layer.
///
/// Two independent channels, both hosted by the core module (the one with the
/// binding map):
///
///   DOWNSTREAM (core -> leaves): core's PUB socket, listening. Every HAN/WAN
///     module dials in as a SUB and subscribes only to the Lexicon path(s) it
///     owns. It will never receive a message for a path it didn't subscribe to.
///
///   UPSTREAM (leaves -> core): core's SUB socket, listening, subscribed to ""
///     (everything) -- intentional: the binding map is the one place the
///     standard wants to see all module traffic. Every HAN/WAN module dials in
///     as a PUB to send its events.
///
/// IMPORTANT LIMITATION: subscription filtering is receive-side only. It stops a
/// leaf module from SEEING traffic it doesn't own; it does NOT stop a
/// compromised leaf from PUBLISHING under a path it doesn't own (NNG's pub
/// socket doesn't check what topic you send under). That's a separate problem --
/// solving it needs per-connection authentication (e.g. mutual TLS, or signed
/// messages checked by the core before acting on them), which this POC does not
/// implement. Don't mistake "leaves can't see each other's traffic" for "leaves
/// are authenticated" -- they're different guarantees.
///
/// Also note: because the core never blindly re-publishes what it receives
/// (every downstream send is synthesized by the binding map / a service object's
/// on_get, not proxied), leaf modules are structurally unable to reach each
/// other directly even ignoring subscriptions -- everything downstream is
/// mediated by the core's own application logic. That's a nice side effect of
/// the hub design, not something pub/sub gives you by itself.
///
/// Memory/ownership: a hes_bus_t owns its two nng sockets and nothing else; the
/// component that opened it (hub or leaf) closes it with hes_bus_close().
///
/// Threading: no internal synchronization -- a handle is driven from the one
/// event loop that owns it, blocking in hes_bus_recv() for at most its timeout.
///
#ifndef OPENHES_SRC_COMMON_HES_BUS_H
#define OPENHES_SRC_COMMON_HES_BUS_H

#include "hes_common.h"

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>

/// How many Lexicon paths one leaf may subscribe to (hes_bus_leaf_open()).
#define HES_BUS_MAX_TOPICS 16

////////////////////////////////////////////////////////////////////////////////
/// The two NNG sockets of one bus handle. Which one sends and which one
/// receives depends on the role -- see the file header, and the two open
/// functions below.
typedef struct hes_bus {
    nng_socket pub_sock;  ///< For sending
    nng_socket sub_sock;  ///< For receiving
} hes_bus_t;

////////////////////////////////////////////////////////////////////////////////
/// Core/hub module: opens and LISTENs on both endpoints.
///
/// @param bus The handle to fill in.
/// @param pub_url Where leaves dial in as SUB to receive downstream traffic.
/// @param sub_url Where leaves dial in as PUB to send upstream traffic. The
///        hub subscribes to everything on this socket (see the file header
///        for why).
/// @return 0 on success, an NNG error code on failure.
int hes_bus_hub_open(hes_bus_t* bus, const char* pub_url, const char* sub_url);

////////////////////////////////////////////////////////////////////////////////
/// Leaf module (HAN/WAN interface module, or any peripheral module): DIALs
/// into the hub's two endpoints, retrying until the core comes up.
///
/// @param bus The handle to fill in.
/// @param hub_pub_url The hub's pub_url (this leaf's socket connects as SUB).
/// @param hub_sub_url The hub's sub_url (this leaf's socket connects as PUB).
/// @param topics The Lexicon path(s) this leaf OWNS -- it will only ever
///        receive messages whose path matches one of these prefixes. At most
///        HES_BUS_MAX_TOPICS entries are used.
/// @param n_topics Number of entries in topics; 0 for a send-only leaf that
///        expects no downstream traffic at all.
/// @return 0 on success, an NNG error code on failure.
int hes_bus_leaf_open(hes_bus_t* bus,
                      const char* hub_pub_url,
                      const char* hub_sub_url,
                      const char** topics,
                      int n_topics);

////////////////////////////////////////////////////////////////////////////////
/// Sends one message, always on bus->pub_sock.
///
/// @param bus The bus handle.
/// @param msg The message to send; its timestamp is stamped here (never by the
///        caller), so no module can forget or forge it.
/// @return 0 on success, an NNG error code on failure.
int hes_bus_send(hes_bus_t* bus, const hes_clme_msg_t* msg);

////////////////////////////////////////////////////////////////////////////////
/// Receives one message, always on bus->sub_sock. Only messages matching a
/// subscribed topic ever arrive here -- see the file header.
///
/// @param bus The bus handle.
/// @param msg Receives the message.
/// @param timeout_ms How long to wait: 0 = don't block, -1 = block forever.
/// @return 0 on success, NNG_ETIMEDOUT on timeout, or another NNG error code.
int hes_bus_recv(hes_bus_t* bus, hes_clme_msg_t* msg, int timeout_ms);

////////////////////////////////////////////////////////////////////////////////
/// Closes both sockets of the handle.
///
/// @param bus The bus handle.
void hes_bus_close(hes_bus_t* bus);

#endif  // #ifndef OPENHES_SRC_COMMON_HES_BUS_H
