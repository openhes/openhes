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
/// @brief Time service (foundational, ISO/IEC 18012-3 11.2.3): the real-time
/// clock, the local time zone and the time source, as three Lexicon objects.
///
/// @details
/// 11.2.3.1: "The time service synchronizes to time (and date) from a range of
/// time sources. This service supplies real time information and other related
/// information to other services." Figure 12 shows five blocks; three of them
/// are Lexicon objects and the other two feed them:
///
///   real time clock            -> the 'rt' object   (/lx/ob/so/ti/rt/st/cv)
///   time zone & daylight map   -> the 'tz' object   (/lx/ob/so/ti/tz/st/cv)
///   time source map            -> the 'st' object   (/lx/ob/so/ti/st/mp/cv)
///   time source & sync         -> services/time/sntp.h   (writes hes_clock)
///   time stamping              -> common/hes_clock.h     (stamps every message)
///
/// That is why 11.2.3 looks like so many tables: it is THREE functional objects
/// (11.2.3.2, 11.2.3.3, 11.2.3.4), and each one has an Object-address table plus
/// a pre-market configurationData, a post-market configurationData and an
/// interactiveData table -- twelve tables, Tables 45 to 56.
///
/// ---------------------------------------------------------------------------
/// Addresses and their tables
///
///   /lx/ob/so/ti/rt/st/cv   Tables 45-48  realTime
///                           'ro' vr dt sz ne nr um sc mp ar pr rq pt ac cd cp
///                           'po' df mn mx vl rl
///                           'ra' va (secondsSinceEpoch)
///
///   /lx/ob/so/ti/tz/st/cv   Tables 49-52  localTimeZone
///                           'pr' dt sz ne nr um sc mp ar pr rq pt ac cd cp
///                           'po' df mn mx vl rl
///                           'ra' va ("<UTC offset minutes>,<daylight minutes>")
///
///   /lx/ob/so/ti/st/mp/cv   Tables 53-56  sourceOfTime
///                           'pr' dt sz ne nr sc mp ar pr rq pt ac cd cp
///                           'po' df mn mx vl rl
///                           'ra' va (time source code: 'nt', 'lx', ...)
///
/// All three answer with ONE ';'-separated 'transCode=value' payload covering
/// every table that shares their address, exactly as the identification service
/// does (see services/id/id.h). Reach a single datum by appending its transCode
/// to the address, or name several with '?da='; the router in
/// common/hes_dispatch.c does the selecting. 18012-4 5.2.9.3.4 makes
/// configurationData and metaData non-updatable by a client, so PUT is logged
/// and ignored here -- that is the standard's rule, not a shortcut.
///
/// 'va' is the value of the currentValue property, which is what a binding-map
/// input's 'ip' of "va" means (18012-3 Table 26), so a rule can take the time
/// straight out of this object.
///
/// ---------------------------------------------------------------------------
/// What is deliberately not here
///
///   - 'cc' (cursorCoordinateInfo): a UI cursor position. This gateway has no
///     user interface, so the standard's own "not applicable" (the empty value
///     its tables use for 'mm' and 'rd') is applied and the datum is omitted
///     rather than fabricated.
///   - 'mm' and 'rd' (momentary / radio buttons): preassigned empty in Tables 46,
///     50 and 54 -- "n/a" for a time service -- so they are omitted too.
///   - 'um' in Table 54: preassigned empty there as well.
///   - 'mn'/'mx' in Table 55: these bound a *value*, and 'va' is an enumerated
///     source code, so a numeric range does not apply and the pair is omitted.
///
/// The gateway never writes the host's clock (that needs CAP_SYS_TIME, and it
/// would make every timer in the process jump). The sync thread publishes a
/// correction through common/hes_clock.h instead, and this service reports the
/// corrected time. See time_sync.h.

#ifndef OPENHES_SRC_SERVICES_TIME_TIME_H
#define OPENHES_SRC_SERVICES_TIME_TIME_H

#include "common/service_object.h"
#include "time_sync.h"

/// How many service objects time_service_create() produces (11.2.3.2/3/4).
#define TIME_SERVICE_OBJECT_COUNT 3

/// Position of each functional object in the array time_service_create() fills.
#define TIME_OBJECT_REALTIME 0
#define TIME_OBJECT_LOCALTZ 1
#define TIME_OBJECT_SOURCE 2

////////////////////////////////////////////////////////////////////////////////
/// Answers "what is the current sync state?" -- normally time_sync_get(), but any
/// provider will do, which is what lets the unit tests drive this service with a
/// fixed snapshot and no network.
///
/// @param ctx  Passed through to the snapshot function.
/// @param out  Receives the current snapshot. The caller owns it and may modify it.
typedef void (*time_snapshot_fn)(void* ctx, time_snapshot_t* out);

////////////////////////////////////////////////////////////////////////////////
/// How the objects learn the current time and zone.
typedef struct time_service_config {
    time_snapshot_fn snapshot;  ///< NULL = never synced (host clock, unknown zone)
    void* snapshot_ctx;         ///< passed through to snapshot()
} time_service_config_t;

////////////////////////////////////////////////////////////////////////////////
/// Build every time-service object -- one per Lexicon address.
///
/// Never fails for environmental reasons: with no Internet, no NTP reply and no
/// resolvable zone the objects still answer, reporting the host clock, the
/// Table 51 default zone and the 'lx' (local crystal) time source.
///
/// @param cfg     Snapshot provider; NULL means "never synced".
/// @param out     Receives the created objects, in the order of the
///                TIME_OBJECT_* indices above.
/// @param max_out Capacity of out; at least TIME_SERVICE_OBJECT_COUNT.
/// @return The number of objects written to out, or -1 on error (nothing is
///         written). The caller owns each object: so->destroy(so), then free(so).
int time_service_create(const time_service_config_t* cfg, service_object_t** out, int max_out);

#endif  // #ifndef OPENHES_SRC_SERVICES_TIME_TIME_H
