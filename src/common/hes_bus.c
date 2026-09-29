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
/// @brief Implementation of the internal HES-CLME event bus (hes_bus.h): hub and
/// leaf sockets, the dial retry, and send/recv.
///
/// @details
/// Two halves, matching the two roles hes_bus.h describes:
///
///   - hes_bus_hub_open(): the core module's side -- PUB listening for
///     downstream, SUB subscribed to "" for upstream.
///
///   - hes_bus_leaf_open(): an interface module's side -- SUB subscribed to the
///     paths it owns, PUB for its own events. Both dial out with a retry loop
///     (50 attempts, 200 ms apart) so a module tolerates the core starting after
///     it.
///
/// hes_bus_send() stamps every outgoing message with hes_clock_now_ns() -- 18012-3
/// 11.2.3.1 requires a time stamp on every HES-CLME message -- and puts it on the
/// one channel that reaches the intended peer.
///
/// Memory/ownership: the sockets live in the caller's hes_bus_t; no heap here.
///
/// Threading: no locking. A handle is meant to be driven from the single event
/// loop that owns it.

#include "hes_bus.h"
#include "hes_clock.h"

#include <log.h>

#include <stdio.h>
#include <string.h>

////////////////////////////////////////////////////////////////////////////////
/// Dials one endpoint, retrying so a leaf tolerates starting before the core.
///
/// @param sock The socket to dial with.
/// @param url The endpoint to dial.
/// @return 0 once connected, or the last NNG error after the retries run out.
static int dial_with_retry(nng_socket sock, const char* url)
{
    int rv = 0;
    for (int attempt = 0; attempt < 50; attempt++) {
        rv = nng_dial(sock, url, NULL, 0);
        if (rv == 0) {
            return 0;
        }

        nng_msleep(200);
    }

    fprintf(stderr, "hes_bus: nng_dial(%s) failed after retries: %s\n", url, nng_strerror(rv));
    return rv;
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

int hes_bus_hub_open(hes_bus_t* bus, const char* pub_url, const char* sub_url)
{
    int rv = nng_pub0_open(&bus->pub_sock);
    if (rv != 0) {
        fprintf(stderr, "hes_bus_hub_open: nng_pub0_open failed: %s\n", nng_strerror(rv));
        return rv;
    }

    rv = nng_listen(bus->pub_sock, pub_url, NULL, 0);
    if (rv != 0) {
        fprintf(stderr, "hes_bus_hub_open: nng_listen(%s) failed: %s\n", pub_url, nng_strerror(rv));
        nng_close(bus->pub_sock);
        return rv;
    }

    rv = nng_sub0_open(&bus->sub_sock);
    if (rv != 0) {
        fprintf(stderr, "hes_bus_hub_open: nng_sub0_open failed: %s\n", nng_strerror(rv));
        nng_close(bus->pub_sock);
        return rv;
    }

    // Hub subscribes to everything -- see hes_bus.h for why this is
    // intentional (the binding map is the trusted, authorized point
    // that's supposed to see all module traffic) rather than an
    // oversight. */

    rv = nng_socket_set(bus->sub_sock, NNG_OPT_SUB_SUBSCRIBE, "", 0);
    if (rv != 0) {
        fprintf(stderr, "hes_bus_hub_open: subscribe-all failed: %s\n", nng_strerror(rv));
    }

    rv = nng_listen(bus->sub_sock, sub_url, NULL, 0);
    if (rv != 0) {
        fprintf(stderr, "hes_bus_hub_open: nng_listen(%s) failed: %s\n", sub_url, nng_strerror(rv));
        nng_close(bus->pub_sock);
        nng_close(bus->sub_sock);
        return rv;
    }

    return 0;
}

int hes_bus_leaf_open(hes_bus_t* bus,
                      const char* hub_pub_url,
                      const char* hub_sub_url,
                      const char** topics,
                      int n_topics)
{
    // Leaf's SUB connects to the hub's PUB (downstream).
    int rv = nng_sub0_open(&bus->sub_sock);
    if (rv != 0) {
        fprintf(stderr, "hes_bus_leaf_open: nng_sub0_open failed: %s\n", nng_strerror(rv));
        return rv;
    }

    if (n_topics == 0) {
        // No topics owned -- subscribe to nothing, matching nothing.
        // (Not the same as the hub's subscribe-all: this leaf simply
        // expects no downstream traffic addressed to it.) */
    } else {
        if (n_topics > HES_BUS_MAX_TOPICS) {
            n_topics = HES_BUS_MAX_TOPICS;
        }
        for (int i = 0; i < n_topics; i++) {
            rv = nng_socket_set(bus->sub_sock, NNG_OPT_SUB_SUBSCRIBE, topics[i], strlen(topics[i]));
            if (rv != 0) {
                fprintf(stderr, "hes_bus_leaf_open: subscribe('%s') failed: %s\n", topics[i],
                        nng_strerror(rv));
            } else {
                fprintf(stderr, "hes_bus_leaf_open: subscribed to topic '%s'\n", topics[i]);
            }
        }
    }

    rv = dial_with_retry(bus->sub_sock, hub_pub_url);
    if (rv != 0) {
        nng_close(bus->sub_sock);
        return rv;
    }

    // Leaf's PUB connects to the hub's SUB (upstream).
    rv = nng_pub0_open(&bus->pub_sock);
    if (rv != 0) {
        fprintf(stderr, "hes_bus_leaf_open: nng_pub0_open failed: %s\n", nng_strerror(rv));
        nng_close(bus->sub_sock);
        return rv;
    }

    rv = dial_with_retry(bus->pub_sock, hub_sub_url);
    if (rv != 0) {
        nng_close(bus->sub_sock);
        nng_close(bus->pub_sock);
        return rv;
    }

    return 0;
}

int hes_bus_send(hes_bus_t* bus, const hes_clme_msg_t* msg)
{
    // Stamp on the way out rather than trusting every sender to do it: the bus
    // is the one place all HES-CLME traffic passes through, so this is the only
    // spot where 18012-3's "every HES-CLME message" can actually be guaranteed.
    // The copy also keeps the caller's buffer const.
    hes_clme_msg_t stamped = *msg;
    stamped.timestamp_ns = hes_clock_now_ns();

    int rv = nng_send(bus->pub_sock, (void*)&stamped, sizeof(stamped), 0);
    if (rv != 0) {
        log_error("nng_send failed: %s", nng_strerror(rv));
    }
    return rv;
}

int hes_bus_recv(hes_bus_t* bus, hes_clme_msg_t* msg, int timeout_ms)
{
    size_t sz = sizeof(*msg);
    nng_duration d = (timeout_ms < 0) ? NNG_DURATION_INFINITE : (nng_duration)timeout_ms;
    nng_socket_set_ms(bus->sub_sock, NNG_OPT_RECVTIMEO, d);
    return nng_recv(bus->sub_sock, msg, &sz, 0);
}

void hes_bus_close(hes_bus_t* bus)
{
    nng_close(bus->pub_sock);
    nng_close(bus->sub_sock);
}
