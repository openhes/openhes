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
/// @brief The service-object interface: state plus behaviour, in the
/// struct-of-function-pointers idiom (ISO/IEC 15045-4-1 Annex A.4; ISO/IEC
/// 18012-3).
///
/// @details
/// A "service object" here is exactly what ISO/IEC 15045-4-1 Annex A.4
/// and ISO/IEC 18012-3 describe it to be: state (properties/config
/// data) PLUS behaviour (controller/processor functions that answer
/// get/put/subscribe and, for some objects, run autonomously). It is
/// *not* a plain data struct -- see the struct-of-function-pointers
/// pattern below, which is the idiomatic-C way to give a struct
/// behaviour without a class system.
///
/// Every service object in this POC lives inside the single
/// core service module process (one daemon, cooperative dispatch by
/// Lexicon path) -- no per-object thread/process is needed.

#ifndef OPENHES_SRC_COMMON_HES_SERVICE_OBJECT_H
#define OPENHES_SRC_COMMON_HES_SERVICE_OBJECT_H

#include "hes_bus.h"
#include "hes_common.h"

////////////////////////////////////////////////////////////////////////////////
/// One service, reachable at one Lexicon address. The struct is opaque to
/// callers except for `path` and `state`.
typedef struct service_object service_object_t;

struct service_object {
    char path[HES_PATH_MAX];  ///< Lexicon address this object answers to
    void* state;              ///< Opaque pointer to the object's own struct

    /// Handle a "get". Convention: the caller pre-populates *out with
    /// the INCOMING request's query and payload (verb/path already
    /// set too) before calling on_get, so objects that need to inspect
    /// the request (e.g. crypto_service reading "ei=" out of the query,
    /// or a payload to transform) can do so via *out itself; on_get
    /// then overwrites out->payload with the response. Objects that
    /// don't need the request (id_service, time_service) simply ignore
    /// whatever was there and overwrite it.
    void (*on_get)(service_object_t* so, hes_clme_msg_t* out);

    /// Handle a "put": apply *in's payload to internal state.
    void (*on_put)(service_object_t* so, const hes_clme_msg_t* in);

    /// Optional: called periodically from the module's main loop so the
    /// object can do autonomous work (e.g. the time service ticking,
    /// or publishing an unsolicited event-report to subscribers).
    /// May be NULL.
    void (*tick)(service_object_t* so, hes_bus_t* bus);

    void (*destroy)(service_object_t* so);  ///< Frees so->state; may be NULL
};

#endif  // #ifndef OPENHES_SRC_COMMON_HES_SERVICE_OBJECT_H
