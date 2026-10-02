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
/// @brief Implementation of the time service (time.h): the three functional
/// objects, their twelve tables, and the realTime event-report.
///
/// @details
/// One object per functional object of Figure 12 -- realTime ('rt'),
/// localTimeZone ('tz') and sourceOfTime ('st') -- each answering a GET with a
/// single ';'-separated 'transCode=value' payload covering every table that shares
/// its address, exactly as the identification service does.
///
/// The tables the installer fills in, which the standard leaves blank, are given
/// documented values here (the TIME_* constants); 'vr', 'sz' and 'mx' are
/// preassigned or derived from the values.
///
/// The realTime object also emits an unsolicited event-report every
/// REALTIME_EVENT_PERIOD_S seconds, so a subscriber sees the clock move without
/// polling for it.

#include "time.h"

#include "common/hes_clock.h"
#include "common/hes_common.h"

#include <log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/// Table 46 'vr': the version of this service, which the standard preassigns.
#define TIME_VERSION "1.0.0"

/// Table 46 'sz': the longest value the data type can produce. Table 47's 'mx'
/// is 4294967295, which is ten digits.
#define REALTIME_VALUE_SIZE 10

/// Table 50 'sz': the longest Table 52 pair, e.g. "-1440,-1440".
#define LOCALTZ_VALUE_SIZE 12

/// Table 54 'sz': the time source is a two-letter code (Table 56).
#define SOURCE_VALUE_SIZE 2

/// Table 47 'mx': the largest secondsSinceEpoch the standard admits.
#define REALTIME_MAX_SECONDS 4294967295ULL

/// How often the realTime object emits an unsolicited event-report.
#define REALTIME_EVENT_PERIOD_S 5

////////////////////////////////////////////////////////////////////////////////
/// The tables the installer fills in, which the standard leaves blank, are given
/// documented values here (Tables 46, 50 and 54 preassign no 'ar', 'pr', 'sz',
/// 'sc', 'cd' or 'cp'):
///
///   ar / pr = 100  accuracy and precision as percentages: the clock is exact at
///                  the one-second resolution of this data type, whether it comes
///                  from NTP or from the host clock.
///   sc      = 0    valueScale: the value is not scaled (10^0).
///   cd      = 'no' collectedDataType: the time service collects nothing about
///                  the user, so there is no data to erase (ISO/IEC 15045-3-1).
///   cp      = 0    collectedDataParameter: not applicable, since 'cd' is 'no'.

////////////////////////////////////////////////////////////////////////////////
/// State shared by all three objects (they are three doors onto one service).
typedef struct time_state {
    int refs;
    time_snapshot_fn snapshot;
    void* snapshot_ctx;

    /// When we last reported the clock, and what we last reported about the zone
    /// and the source, so those two are published on change and not on a
    /// cadence.
    time_t last_event;
    int have_tz;
    char tz_name[GEO_TZ_NAME_MAX];
    int tz_std;
    int tz_dst;
    int have_source;
    int source_is_ntp;
} time_state_t;

////////////////////////////////////////////////////////////////////////////////
/// Copy the current sync snapshot, or a zeroed one when no provider is set.
static void sample_state(time_state_t* st, time_snapshot_t* out)
{
    memset(out, 0, sizeof(*out));
    if (st->snapshot != NULL) {
        st->snapshot(st->snapshot_ctx, out);
    }
}

////////////////////////////////////////////////////////////////////////////////
/// Tables 46, 47 and 48: the realTime object's pre-market configurationData
/// (memoryType 'ro'), post-market configurationData ('po') and interactiveData
/// ('ra'), in that order.
static void build_realtime_payload(char* buf, size_t cap)
{
    long long now = (long long)hes_clock_now();
    if (now < 0) {
        now = 0;
    }
    // Table 47 'mx' is a real bound, not decoration: the reported value has to
    // stay inside the range the table declares.
    if ((unsigned long long)now > REALTIME_MAX_SECONDS) {
        now = (long long)REALTIME_MAX_SECONDS;
    }

    int n = snprintf(buf, cap,
                     "vr=" TIME_VERSION
                     ";dt=ss;sz=%d;ne=1;nr=0;um=se;sc=0;mp=me;ar=100;"
                     "pr=100;rq=op;pt=tm;ac=in;cd=no;cp=0;df=0;mn=0;mx=%llu;vl=0;rl=0;va=%lld",
                     REALTIME_VALUE_SIZE, (unsigned long long)REALTIME_MAX_SECONDS, now);
    if (n < 0 || (size_t)n >= cap) {
        log_error("time: the realTime answer does not fit %d bytes", (int)cap);
    }
}

////////////////////////////////////////////////////////////////////////////////
/// Tables 50, 51 and 52: the localTimeZone object.
static void build_localtz_payload(time_state_t* st, char* buf, size_t cap)
{
    time_snapshot_t snap;
    sample_state(st, &snap);

    // Table 52 'va': "Difference to UTC (minutes), daylight adjustment
    // (minutes)", e.g. -240,+60 for UTC-4 with an hour of daylight saving. Until
    // the zone is resolved, Table 51's 'df' default "0,0" is reported -- that is
    // the standard's own value for "nothing better is known".
    int std_min = snap.tz_valid ? snap.std_offset_min : 0;
    int dst_min = snap.tz_valid ? snap.dst_offset_min : 0;

    int n = snprintf(buf, cap,
                     "dt=ai,ai;sz=%d;ne=2;nr=0;um=mi,mi;sc=0;mp=pr;ar=100;pr=100;rq=op;"
                     "pt=tm;ac=in;cd=no;cp=0;df=0,0;mn=-1440,-1440;mx=1440,1440;vl=0;rl=0;"
                     "va=%d,%d",
                     LOCALTZ_VALUE_SIZE, std_min, dst_min);
    if (n < 0 || (size_t)n >= cap) {
        log_error("time: the localTimeZone answer does not fit %d bytes", (int)cap);
    }
}

////////////////////////////////////////////////////////////////////////////////
/// Tables 54, 55 and 56: the sourceOfTime object.
static void build_source_payload(time_state_t* st, char* buf, size_t cap)
{
    time_snapshot_t snap = {0};
    sample_state(st, &snap);

    // Table 56 'va' is the time source reference: 'nt' (internet, NTP) once a
    // sync has succeeded, otherwise 'lx' (local crystal), which is also Table
    // 55's 'df' default and where an offline gateway stays.
    const char* code = time_sync_source_code(&snap);

    int n = snprintf(buf, cap,
                     "dt=sl;sz=%d;ne=1;nr=0;sc=0;mp=pr;ar=100;pr=100;rq=op;pt=tm;ac=in;"
                     "cd=no;cp=0;df=lx;vl=0;rl=0;va=%s",
                     SOURCE_VALUE_SIZE, code);
    if (n < 0 || (size_t)n >= cap) {
        log_error("time: the sourceOfTime answer does not fit %d bytes", (int)cap);
    }
}

////////////////////////////////////////////////////////////////////////////////
/// The time service is not client-writable, so PUTs are ignored. The objects'
/// answers come from the time source and from provisioning, not from a client.
///
/// Every table of this service is configurationData ('ro', 'pr', 'po') or
/// metaData, and 18012-4 5.2.9.3.4 lets a client update neither. The values
/// come from the time source and from provisioning, so there is nothing here
/// to apply -- say so rather than pretend the value was taken.
static void time_on_put(service_object_t* so, const hes_clme_msg_t* in)
{
    (void)so;
    log_info("PUT to %s ignored (the time service is not client-writable)", in->path);
}

////////////////////////////////////////////////////////////////////////////////
/// Sends one unsolicited event-report carrying an address's answer.
///
/// @param bus the bus to send on
/// @param path the address to report (one of the three time service addresses)
/// @param payload the answer to send, which the caller has built
static void publish_event(hes_bus_t* bus, const char* path, const char* payload)
{
    hes_clme_msg_t msg = {0};
    msg.verb = HES_VERB_EVENT;
    hes_msg_set_path(&msg, path);

    // Which datum this report carries: the object's currentValue ('va').
    hes_strlcpy(msg.query, sizeof(msg.query), "va");
    hes_msg_set_payload_str(&msg, payload);
    hes_bus_send(bus, &msg);
}

////////////////////////////////////////////////////////////////////////////////
/// Autonomous work for the realTime object.
///
/// The time service "DOES do work on its own": it reports the clock on a cadence
/// so the binding map can evaluate time-based rules ("is it after 18:00?")
/// without polling, and reports the zone and the time source whenever they
/// change.
static void realtime_tick(service_object_t* so, hes_bus_t* bus)
{
    time_state_t* st = (time_state_t*)so->state;
    char buf[HES_PAYLOAD_MAX] = {0};

    // The realTime object ticks on a cadence, and it also publishes the other two
    // addresses' changes. The other two objects do not tick, and they do not
    // publish the realTime address' changes.
    time_t now = time(NULL);
    if (st->last_event == 0 || now - st->last_event >= REALTIME_EVENT_PERIOD_S) {
        st->last_event = now;
        build_realtime_payload(buf, sizeof(buf));
        publish_event(bus, HES_LX_TIME_REALTIME_CV, buf);
    }

    time_snapshot_t snap = {0};
    sample_state(st, &snap);

    // The localTimeZone object is published only when the zone changes, and the
    // sourceOfTime object is published only when the source changes. The
    // realTime object does not publish either of those addresses' changes, and
    // the other two objects do not tick, so they cannot publish anything.
    if (snap.tz_valid && (!st->have_tz || strcmp(st->tz_name, snap.tz_name) != 0 ||
                          st->tz_std != snap.std_offset_min || st->tz_dst != snap.dst_offset_min)) {
        st->have_tz = 1;
        hes_strlcpy(st->tz_name, sizeof(st->tz_name), snap.tz_name);
        st->tz_std = snap.std_offset_min;
        st->tz_dst = snap.dst_offset_min;
        build_localtz_payload(st, buf, sizeof(buf));
        publish_event(bus, HES_LX_TIME_LOCALTZ_CV, buf);
    }

    // The sourceOfTime object is published only when the source changes. The
    // realTime object does not publish that address' changes, and the
    // localTimeZone object does not tick, so it cannot publish anything.
    if (!st->have_source || st->source_is_ntp != snap.ntp_valid) {
        st->have_source = 1;
        st->source_is_ntp = snap.ntp_valid;
        build_source_payload(st, buf, sizeof(buf));
        publish_event(bus, HES_LX_TIME_SOURCE_CV, buf);
    }
}

////////////////////////////////////////////////////////////////////////////////
/// Answers a GET with the realTime object's tables: the clock and how accurate
/// we believe it is, from the sync thread's latest snapshot.
///
/// @param so  The service object (unused: the snapshot is process-wide).
/// @param out Receives the reply payload.
static void realtime_on_get(service_object_t* so, hes_clme_msg_t* out)
{
    (void)so;
    char buf[HES_PAYLOAD_MAX] = {0};
    build_realtime_payload(buf, sizeof(buf));
    hes_msg_set_payload_str(out, buf);
}

////////////////////////////////////////////////////////////////////////////////
/// Answers a GET with the localTimeZone tables: the zone name and the Table 52
/// offset pair, as last resolved by the sync thread.
///
/// @param so  The service object, which carries the shared state.
/// @param out Receives the reply payload.
static void localtz_on_get(service_object_t* so, hes_clme_msg_t* out)
{
    char buf[HES_PAYLOAD_MAX] = {0};
    build_localtz_payload((time_state_t*)so->state, buf, sizeof(buf));
    hes_msg_set_payload_str(out, buf);
}

////////////////////////////////////////////////////////////////////////////////
/// Answers a GET with the sourceOfTime tables: which time source is in use (an
/// NTP server, or the local crystal) and its status.
///
/// @param so  The service object, which carries the shared state.
/// @param out Receives the reply payload.
static void source_on_get(service_object_t* so, hes_clme_msg_t* out)
{
    char buf[HES_PAYLOAD_MAX] = {0};
    build_source_payload((time_state_t*)so->state, buf, sizeof(buf));
    hes_msg_set_payload_str(out, buf);
}

////////////////////////////////////////////////////////////////////////////////
/// Releases the service state. One state is shared by the three objects
/// (realTime, localTimeZone, sourceOfTime), so only the last one out frees it.
///
/// @param so The service object being destroyed.
static void time_destroy(service_object_t* so)
{
    time_state_t* st = (time_state_t*)so->state;
    if (st == NULL) {
        return;
    }
    so->state = NULL;

    // Shared by the three objects, so only the last one out frees it.
    if (--st->refs > 0) {
        return;
    }
    free(st);
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

int time_service_create(const time_service_config_t* cfg, service_object_t** out, int max_out)
{
    if (out == NULL || max_out < TIME_SERVICE_OBJECT_COUNT) {
        log_error("need room for %d service objects", TIME_SERVICE_OBJECT_COUNT);
        return -1;
    }

    time_state_t* st = calloc(1, sizeof(*st));
    if (st == NULL) {
        log_error("failed to allocate memory for time service state");
        return -1;
    }

    if (cfg != NULL) {
        st->snapshot = cfg->snapshot;
        st->snapshot_ctx = cfg->snapshot_ctx;
    }

    // One object per Lexicon address: same service, three doors. Only realTime
    // ticks, and it also publishes the other two addresses' changes.
    static const struct time_address {
        const char* path;
        void (*on_get)(service_object_t*, hes_clme_msg_t*);
        void (*tick)(service_object_t*, hes_bus_t*);
    } ADDRESSES[TIME_SERVICE_OBJECT_COUNT] = {
            {HES_LX_TIME_REALTIME_CV, realtime_on_get, realtime_tick},
            {HES_LX_TIME_LOCALTZ_CV, localtz_on_get, NULL},
            {HES_LX_TIME_SOURCE_CV, source_on_get, NULL},
    };

    for (int i = 0; i < TIME_SERVICE_OBJECT_COUNT; i++) {
        service_object_t* so = calloc(1, sizeof(*so));
        if (so == NULL) {
            log_error("failed to allocate memory for time service object");
            for (int j = 0; j < i; j++) {
                out[j]->destroy(out[j]);
                free(out[j]);
                out[j] = NULL;
            }
            free(st);
            return -1;
        }

        hes_strlcpy(so->path, sizeof(so->path), ADDRESSES[i].path);
        so->state = st;
        so->on_get = ADDRESSES[i].on_get;
        so->on_put = time_on_put;
        so->tick = ADDRESSES[i].tick;
        so->destroy = time_destroy;

        st->refs++;
        out[i] = so;
    }

    return TIME_SERVICE_OBJECT_COUNT;
}
