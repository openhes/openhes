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
/// @brief Implementation of the zone lookup (geo_tz.h): ask a web service which
/// zone the host is in, then turn that name into the Table 52 pair.
///
/// @details
/// The two halves keep the durable answer and the volatile one apart:
///
///   - geo_tz_detect(): one HTTPS GET to a zone provider, parsed with Jansson. It
///     returns an IANA name (e.g. "Asia/Taipei"), not a bare offset -- an offset
///     cannot tell you what the offset will be after the next DST switch. The
///     endpoints come from the caller (--tz-urls), with built-in defaults.
///
///   - geo_tz_offsets(): turns that name into (standard, daylight) minutes using
///     the host's own tz database, with no further network traffic.
///
/// The TZ probe is process-global state, so its window is kept to a few
/// microseconds and enters through the sync thread only -- see the note in
/// geo_tz.h.
///
/// Memory/ownership: the probe saves and restores the caller's TZ value in a
/// fixed buffer; the JSON tree is Jansson's and is released before returning.
///
/// Threading: not independently safe -- call it from the time service's sync
/// thread, as the header explains.

// tm_gmtoff (a BSD/glibc extension) and timegm() need the default feature set;
// a compiler invoked with a strict -std=c11 would hide both. This must come
// before every other include.
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "geo_tz.h"

#include "common/hes_clock.h"
#include "common/hes_https.h"

#include <jansson.h>
#include <log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/// Where the tz database lives when it is installed as files rather than as the
/// library-linked default (glibc's TZDIR).
#define TZ_DIR "/usr/share/zoneinfo/"

#define TZ_ENV_SAVE_MAX 256

////////////////////////////////////////////////////////////////////////////////
/// True when 'name' is plausibly an IANA zone name: "Area/Location", or one of
/// the handful of single-word zones such as "UTC". Deliberately strict -- this
/// value goes into the TZ environment variable.
///
/// @param name
static int zone_name_is_sane(const char* name)
{
    if (name == NULL || *name == '\0') {
        return 0;
    }

    size_t len = strlen(name);
    if (len >= GEO_TZ_NAME_MAX) {
        return 0;
    }

    for (const char* p = name; *p != '\0'; p++) {
        int ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
                 *p == '/' || *p == '_' || *p == '-' || *p == '+';
        if (!ok) {
            return 0;
        }
    }

    // No leading, trailing or doubled separators.
    if (name[0] == '/' || name[len - 1] == '/') {
        return 0;
    }

    if (strstr(name, "//") != NULL) {
        return 0;
    }

    return 1;
}

////////////////////////////////////////////////////////////////////////////////
/// Copies a zone name out, rejecting anything that is not a sane one.
///
/// @param src
/// @param buf
/// @param buf_size
/// @return
static int adopt_zone_name(const char* src, char* buf, size_t buf_size)
{
    if (!zone_name_is_sane(src)) {
        return -1;
    }

    if (strlen(src) >= buf_size) {
        return -1;
    }

    snprintf(buf, buf_size, "%s", src);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Extracts a zone name from a path such as "/usr/share/zoneinfo/Asia/Tokyo".
///
/// @param path
/// @param buf
/// @param buf_size
/// @return
static int zone_from_path(const char* path, char* buf, size_t buf_size)
{
    const char* marker = strstr(path, "zoneinfo/");
    const char* zone = marker ? marker + sizeof("zoneinfo/") - 1 : path;
    return adopt_zone_name(zone, buf, buf_size);
}

////////////////////////////////////////////////////////////////////////////////
/// Midday UTC on the first of 'mon' (0 = January) -- far from any date line.
///
/// @param year
/// @param mon
/// @return
static time_t first_of_month_utc(int year, int mon)
{
    struct tm t = {0};
    t.tm_year = year - 1900;
    t.tm_mon = mon;
    t.tm_mday = 1;
    t.tm_hour = 12;
    return timegm(&t);
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

int geo_tz_parse_json(const char* body, const char* key, char* name, size_t cap)
{
    if (body == NULL || name == NULL || cap == 0) {
        return -1;
    }
    name[0] = '\0';

    const char* want = (key != NULL && *key != '\0') ? key : GEO_TZ_DEFAULT_KEY;

    json_error_t err;
    json_t* root = json_loads(body, 0, &err);
    if (root == NULL) {
        log_debug("geo_tz: provider answer is not JSON: %s", err.text);
        return -1;
    }

    // Some providers nest the answer; look one level deep as well.
    json_t* value = json_object_get(root, want);
    if (value == NULL) {
        const char* const containers[] = {"data", "location", NULL};
        for (int i = 0; value == NULL && containers[i] != NULL; i++) {
            json_t* inner = json_object_get(root, containers[i]);
            if (json_is_object(inner)) {
                value = json_object_get(inner, want);
            }
        }
    }

    int rc = -1;
    if (json_is_string(value)) {
        const char* zone = json_string_value(value);
        if (adopt_zone_name(zone, name, cap) == 0) {
            rc = 0;
        } else {
            log_error("geo_tz: provider named an unusable zone '%s'", zone ? zone : "");
        }
    } else {
        log_debug("geo_tz: provider answer has no '%s' string", want);
    }

    json_decref(root);
    if (rc != 0) {
        name[0] = '\0';
    }
    return rc;
}

int geo_tz_detect(const char* url, const char* json_key, char* name, size_t cap, int timeout_ms)
{
    if (name == NULL || cap == 0) {
        return -1;
    }
    name[0] = '\0';

    const char* endpoint = (url != NULL && *url != '\0') ? url : GEO_TZ_DEFAULT_URL;
    char body[HES_HTTPS_BODY_MAX];

    if (hes_https_get(endpoint, body, sizeof(body), NULL, timeout_ms) != 0) {
        return -1;
    }

    return geo_tz_parse_json(body, json_key, name, cap);
}

int geo_tz_system_name(char* name, size_t cap)
{
    if (name == NULL || cap == 0) {
        return -1;
    }
    name[0] = '\0';

    const char* tz = getenv("TZ");
    if (tz != NULL && *tz != '\0') {
        if (*tz == ':') {
            tz++;  // POSIX allows ":/path/to/zone"
        }
        if (*tz != '\0' && zone_from_path(tz, name, cap) == 0) {
            return 0;
        }
    }

    // No usable TZ: distributions point /etc/localtime at the chosen zone.
    char link[512];
    ssize_t n = readlink("/etc/localtime", link, sizeof(link) - 1);
    if (n > 0) {
        link[n] = '\0';
        if (zone_from_path(link, name, cap) == 0) {
            return 0;
        }
    }

    return -1;
}

int geo_tz_offsets(const char* name, int* std_min, int* dst_min)
{
    if (!zone_name_is_sane(name) || std_min == NULL || dst_min == NULL) {
        return -1;
    }

    // Refuse a zone the database does not have. tzset() would otherwise fall
    // back to UTC silently, and a wrong "+00:00" is worse than no answer.
    char path[sizeof(TZ_DIR) + GEO_TZ_NAME_MAX + 1];
    int n = snprintf(path, sizeof(path), TZ_DIR "%s", name);
    if (n <= 0 || (size_t)n >= sizeof(path) || access(path, R_OK) != 0) {
        log_debug("geo_tz: '%s' is not in the system time zone database", name);
        return -1;
    }

    char saved[TZ_ENV_SAVE_MAX];
    saved[0] = '\0';
    const char* current = getenv("TZ");
    int had_tz = current != NULL;
    if (had_tz && strlen(current) < sizeof(saved)) {
        strcpy(saved, current);
    }

    setenv("TZ", name, 1);
    tzset();

    int rc = -1;
    int zone_std = 0;
    int zone_dst = 0;

    time_t now = hes_clock_now();
    struct tm utc;
    if (gmtime_r(&now, &utc) != NULL) {
        int year = utc.tm_year + 1900;
        time_t jan = first_of_month_utc(year, 0);
        time_t jul = first_of_month_utc(year, 6);

        struct tm jan_local;
        struct tm jul_local;
        if (localtime_r(&jan, &jan_local) != NULL && localtime_r(&jul, &jul_local) != NULL) {
            int jan_off = (int)(jan_local.tm_gmtoff / 60);
            int jul_off = (int)(jul_local.tm_gmtoff / 60);
            int jan_dst = jan_local.tm_isdst > 0;
            int jul_dst = jul_local.tm_isdst > 0;

            if (jan_dst && !jul_dst) {
                // Southern hemisphere: January is the summer.
                zone_std = jul_off;
                zone_dst = jan_off - jul_off;
            } else if (jul_dst && !jan_dst) {
                zone_std = jan_off;
                zone_dst = jul_off - jan_off;
            } else if (!jan_dst && !jul_dst) {
                zone_std = jan_off;
                zone_dst = 0;
            } else {
                // Both sampled as daylight saving (unusual rules): report the
                // January offset as standard rather than invent one.
                zone_std = jan_off;
                zone_dst = jul_off - jan_off;
            }
            rc = 0;
        }
    }

    if (had_tz) {
        setenv("TZ", saved, 1);
    } else {
        unsetenv("TZ");
    }
    tzset();

    if (rc != 0) {
        log_error("geo_tz: cannot read the offsets for zone '%s'", name);
        return -1;
    }

    *std_min = zone_std;
    *dst_min = zone_dst;
    return 0;
}
