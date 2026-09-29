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
/// @brief Routing an incoming HES-CLME message to the local service object that
/// owns its address.
///
/// @details
/// Three addressing forms are supported, all from 18012-4:
///
///   1. The object's own address retrieves all of its data:
///
///          get /lx/ob/so/id            ->  "rq=ma;si=ye"
///
///   2. The data-item form appends the datum's transCode to that address
///      (5.2.8.2.2, which does the same with '/lx/ob/uo/li/ll/da/cv/va'):
///
///          get /lx/ob/so/id/rq         ->  "rq=ma"
///
///   3. Several data points can be asked for in one request with the 'da' query
///      (5.2.5.5: "'da': multiple data points"), comma separated:
///
///          get /lx/ob/so/id?da=rq,si   ->  "rq=ma;si=ye"
///
/// Forms 2 and 3 are equivalent here, because form 2 is simply sugar for a
/// one-item 'da' list. Both are handled at this layer rather than in each
/// service, so every object gets them for free.
///
/// A 'da' list that names nothing the object has yields an empty payload, with a
/// diagnostic: the client asked for data that does not exist, and the standard's
/// answer to that is "4.04 Not Found" rather than a silent full dump.
///
/// Memory/ownership: nothing is allocated here. The dispatcher picks an object
/// out of the caller's array and hands the message through.
///
/// Threading: no shared state; called from the module's single bus loop.
///

#ifndef OPENHES_SRC_COMMON_HES_DISPATCH_H
#define OPENHES_SRC_COMMON_HES_DISPATCH_H

#include "common/hes_bus.h"
#include "common/service_object.h"

////////////////////////////////////////////////////////////////////////////////
/// Routes one message to whichever local service object owns its address.
///
/// Whichever form was used, the object answers from its own address; a GET then
/// has its payload trimmed to the requested data points.
///
/// @param bus The event bus to send GET answers on.
/// @param objs The local service objects.
/// @param n_objs How many entries objs holds.
/// @param in The incoming message.
/// @return 1 if the message was addressed to a local object, 0 otherwise.
int dispatch_to_local_objects(hes_bus_t* bus,
                              service_object_t** objs,
                              int n_objs,
                              const hes_clme_msg_t* in);

#endif  // #ifndef OPENHES_SRC_COMMON_HES_DISPATCH_H
