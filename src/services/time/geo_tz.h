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
/// @brief The "time zone & daylight map" block of the time service: detect the
/// host's zone by name, then map it to the Table 52 offset pair.
///
/// @details
/// The block of the time service's block diagram
/// (ISO/IEC 18012-3 11.2.3.1, Figure 12): "After selecting the local time zone,
/// the 'time zone & daylight map' provides the local adjustment compared to UTC."
///
/// 11.2.3.3 asks for exactly two numbers, Table 52 "value 'va'":
///
///     "Difference to UTC (minutes), daylight adjustment (minutes)
///      (e.g.: -240,+60 means that standard time is UTC - 4 hours
///      (-240 minutes), and presently in daylight of +1 hour (60 minutes))"
///
/// So detection has two halves:
///
///   1. geo_tz_detect() asks a web service which zone this host is in, and gets
///      back an IANA name (e.g. "Asia/Taipei") rather than a bare offset. A name
///      is the durable answer: an offset alone cannot tell you what the offset
///      will be after the next daylight-saving switch.
///   2. geo_tz_offsets() turns that name into the standard and daylight pair the
///      table asks for, using the host's own time zone database -- no further
///      network traffic, and it stays correct across DST transitions.
///
/// The zone name may also be supplied by the operator (--time-zone), or taken
/// from the host (/etc/localtime), so the gateway works with no Internet at all.
///
/// Threading: geo_tz_offsets() reads a specific zone by setting TZ and
/// calling tzset(), which is process-global state. It is therefore called only
/// from the time service's sync thread, and the window in which TZ holds the
/// probed zone is kept to a few microseconds (one gmtime_r and two localtime_r
/// calls).
///
/// That window is not entirely free: the log module formats its timestamps in
/// local time, so a log line emitted from another thread inside those
/// microseconds would be stamped in the probed zone instead of the host's.
/// Nothing else in the gateway reads local time -- the bus loop works in UTC --
/// and the worst case is one cosmetic log timestamp every six hours, so this is
/// recorded rather than worked around: glibc exposes no public "offset of zone X"
/// call that avoids the global TZ.

#ifndef OPENHES_SRC_SERVICES_TIME_GEO_TZ_H
#define OPENHES_SRC_SERVICES_TIME_GEO_TZ_H

#include <stddef.h>

/// Longest IANA zone name accepted, e.g. "America/Argentina/Buenos_Aires".
#define GEO_TZ_NAME_MAX 64

/// Default provider: free, keyless, answers {"timezone":"Asia/Taipei",...}.
#define GEO_TZ_DEFAULT_URL "https://ipapi.co/json/"

/// Zone providers to ask, in this order. If one fails, the next one is tried,
/// so a single failing provider does not stop zone detection.
#define GEO_TZ_DEFAULT_URLS "https://ipapi.co/json/,https://worldtimeapi.org/api/ip"

/// JSON key both default providers use for the zone name.
#define GEO_TZ_DEFAULT_KEY "timezone"

////////////////////////////////////////////////////////////////////////////////
/// Asks the provider at 'url' which zone this host is in.
///
/// @param url        https:// endpoint returning JSON; NULL for the default.
/// @param json_key   JSON member holding the zone name; NULL for the default.
/// @param name       Receives the zone name.
/// @param cap        Capacity of name (GEO_TZ_NAME_MAX is enough in practice).
/// @param timeout_ms Budget for the whole request.
/// @return 0 on success, -1 otherwise (reason logged).
int geo_tz_detect(const char* url,
                  const char* json_key,
                  char* buf,
                  size_t buf_size,
                  int timeout_ms);

////////////////////////////////////////////////////////////////////////////////
/// Pure: read the zone name out of a provider's JSON document. No network.
///
/// The name is checked to look like an IANA zone (letters, digits, '_', '-', '+'
/// and '/') before it is accepted, so a hostile or broken provider cannot push
/// arbitrary text into TZ.
///
/// @return 0 on success, -1 otherwise.
int geo_tz_parse_json(const char* body, const char* key, char* buf, size_t buf_size);

////////////////////////////////////////////////////////////////////////////////
/// Fallback with no network: the host's own idea of its zone -- the TZ
/// environment variable, else the zone named by the /etc/localtime symlink.
///
/// @return 0 on success, -1 when the host does not name a zone.
int geo_tz_system_name(char* buf, size_t buf_size);

////////////////////////////////////////////////////////////////////////////////
/// The Table 52 pair for an IANA zone, in minutes, from the system tz database.
///
/// Implemented by sampling the zone at mid-January and mid-July of the current
/// year: whichever sample is not in daylight saving gives the standard offset,
/// and the difference between them is the daylight adjustment. Sampling both
/// halves of the year is what makes this correct in the southern hemisphere too,
/// where January is the summer.
///
/// @param name    IANA zone name, e.g. "Asia/Taipei".
/// @param std_min Receives the standard offset from UTC, in minutes.
/// @param dst_min Receives the daylight adjustment, in minutes (0 when the zone
///                has none).
/// @return 0 on success; -1 when the zone is unknown to the host's tz database
///         (in which case a wrong "+00:00" is NOT reported).
int geo_tz_offsets(const char* name, int* std_min, int* dst_min);

#endif  // #ifndef OPENHES_SRC_SERVICES_TIME_GEO_TZ_H
