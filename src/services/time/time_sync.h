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
/// @brief The time service's two network-driven blocks -- NTP sync and the time
/// zone lookup -- behind one small, thread-owned API.
///
/// @details
/// The two network-driven blocks (ISO/IEC 18012-3 11.2.3.1, Figure 12) behind one
/// small API:
///
///   "time source & sync"      -> sntp.h   (an NTP server, source code 'nt')
///   "time zone & daylight map" -> geo_tz.h (the Table 52 pair)
///
/// Why its own thread: the module's bus loop must never block. A DNS lookup plus
/// a UDP round trip plus (once in a while) a TLS handshake can take seconds; done
/// inline, every one of those seconds would be a second the gateway is not
/// answering HES-CLME. So the work happens here, and the bus loop only ever reads
/// a finished snapshot.
///
/// Sharing is deliberately narrow: one mutex, one snapshot struct, no callbacks
/// into other subsystems. hes_clock is the only global this touches, and that is
/// atomic by design (see common/hes_clock.h).
///
/// Offline behaviour is a first-class case, not an error path: with no Internet
/// the worker logs, leaves 'ntp_valid' at 0 and 'reachable' at 0, the time source
/// stays 'lx' (local crystal) per Table 56, and the gateway keeps running on the
/// host clock exactly as it did before this module existed.

#ifndef OPENHES_SRC_SERVICES_TIME_TIME_SYNC_H
#define OPENHES_SRC_SERVICES_TIME_TIME_SYNC_H

#include "geo_tz.h"

#include <time.h>

/// Public NTP servers, tried in order until one answers.
#define TIME_SYNC_DEFAULT_SERVERS "pool.ntp.org,time.google.com,time.cloudflare.com"

/// Re-sync interval. Drift on a commodity crystal is far slower than this.
#define TIME_SYNC_DEFAULT_NTP_INTERVAL_S 3600L

/// Re-check interval for the time zone: zones change only when the gateway moves.
#define TIME_SYNC_DEFAULT_TZ_INTERVAL_S 21600L

#define TIME_SYNC_DEFAULT_TIMEOUT_MS 4000

////////////////////////////////////////////////////////////////////////////////
/// Everything the time service objects read, in one copyable struct.
typedef struct time_snapshot {
    int ntp_valid;        ///< 1 once a reply passed validation, 0 = never synced
    long long offset_ns;  ///< server time - CLOCK_REALTIME
    long delay_ms;        ///< round-trip delay of the sync that set the offset
    int stratum;          ///< stratum of that server (1..15)
    time_t last_sync;     ///< when the last successful sync happened; 0 = never

    int tz_valid;                   ///< 1 once a zone was resolved
    char tz_name[GEO_TZ_NAME_MAX];  ///< IANA name, e.g. "Asia/Taipei"
    int std_offset_min;             ///< Table 52 'va', first integer
    int dst_offset_min;             ///< Table 52 'va', second integer

    int reachable;  ///< 1 when the last attempt reached the Internet
} time_snapshot_t;

////////////////////////////////////////////////////////////////////////////////
/// How to sync. All fields are optional; zero means "use the default".
typedef struct time_sync_config {
    const char* ntp_servers;  ///< CSV; NULL/empty = TIME_SYNC_DEFAULT_SERVERS
    long ntp_interval_s;      ///< 0 = TIME_SYNC_DEFAULT_NTP_INTERVAL_S
    int ntp_enabled;          ///< 0 = never contact an NTP server

    const char* tz_name;  ///< operator override; skips detection entirely
    const char* tz_urls;  ///< CSV of providers; NULL/empty = the defaults
    int tz_enabled;       ///< 0 = never contact a provider
    long tz_interval_s;   ///< 0 = TIME_SYNC_DEFAULT_TZ_INTERVAL_S

    int timeout_ms;  ///< per network operation; 0 = the default
} time_sync_config_t;

typedef struct time_sync time_sync_t;

////////////////////////////////////////////////////////////////////////////////
/// Start the sync thread. Never fails because the network is unavailable -- only
/// for local reasons (out of memory, no thread).
///
/// @return The handle, or NULL.
time_sync_t* time_sync_start(const time_sync_config_t* cfg);

////////////////////////////////////////////////////////////////////////////////
/// Copy the current snapshot under the lock.
///
/// Safe with a NULL handle (an all-zero snapshot results, i.e. "never synced"),
/// so callers need no special case when sync was not started.
void time_sync_get(time_sync_t* ts, time_snapshot_t* out);

////////////////////////////////////////////////////////////////////////////////
/// Stop and join the thread, then free the handle. Safe with NULL.
void time_sync_stop(time_sync_t* ts);

////////////////////////////////////////////////////////////////////////////////
/// The Table 56 'va' code naming where the time currently comes from:
/// 'nt' (internet, NTP) once a sync has succeeded, else 'lx' (local crystal),
/// which is where a gateway with no Internet stays forever.
///
/// Static inline because it reads nothing but the snapshot: that keeps the
/// service objects free of any link dependency on the sync thread, which is
/// what lets them be unit-tested on their own.
static inline const char* time_sync_source_code(const time_snapshot_t* snap)
{
    return (snap != NULL && snap->ntp_valid) ? "nt" : "lx";
}

#endif  // #ifndef OPENHES_SRC_SERVICES_TIME_TIME_SYNC_H
