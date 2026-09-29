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
/// @brief Implementation of the time service's sync worker (time_sync.h): one
/// thread that does the NTP round trip and the zone lookup off the bus loop.
///
/// @details
/// The worker owns a snapshot (the offset from NTP, the zone from the provider)
/// plus the schedule for the next attempt, and sleeps on a condition variable
/// between them. The config's strings are copied into the struct, so the caller's
/// buffers only have to outlive the call.
///
/// The intervals are floored (MIN_NTP_INTERVAL_S, MIN_TZ_INTERVAL_S) so a
/// misconfigured caller cannot hammer public infrastructure for no benefit, and
/// IDLE_WAIT_S is how long it sleeps when nothing at all is scheduled.
///
/// Being offline is a first-class case, not an error path: the worker logs, the
/// snapshot stays invalid, the time source stays 'lx', and the gateway keeps
/// running on the host clock -- see time_sync.h.
///
/// Memory/ownership: the worker owns its thread, mutex, condition variable and the
/// copied config strings. hes_clock is the only global it touches, and that is
/// atomic by design.
///
/// Threading: this is the one background thread the core module starts. The bus
/// loop only ever reads the snapshot, under the mutex.

#include "time_sync.h"

#include "common/hes_clock.h"
#include "common/hes_common.h"
#include "sntp.h"

#include <log.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/// Intervals below these would hammer public infrastructure for no benefit: a
/// gateway's crystal drifts far slower than a minute, and it does not move
/// between time zones more than once a day.
#define MIN_NTP_INTERVAL_S 60L
#define MIN_TZ_INTERVAL_S 300L

/// How long to sleep when nothing at all is scheduled.
#define IDLE_WAIT_S 3600

struct time_sync {
    /// Strings are copied in, then cfg is re-pointed at the copies, so the
    /// caller's buffers only have to live for the duration of the call.
    time_sync_config_t cfg;
    char servers[256];
    char tz_urls[256];
    char forced_tz[GEO_TZ_NAME_MAX];

    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int stop;

    time_snapshot_t snap;
};

////////////////////////////////////////////////////////////////////////////////
/// Milliseconds on a monotonic clock, for scheduling.
///
/// @return
static long long monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long long)ts.tv_sec * 1000 + (long long)ts.tv_nsec / 1000000;
}

////////////////////////////////////////////////////////////////////////////////
/// Copies the next comma-separated field out of a CSV list.
///
/// @param cursor Advanced past the field consumed; starts at the list head.
/// @return 1 when a field was read, 0 at the end of the list.
static int next_csv(const char** cursor, char* out, size_t cap)
{
    while (**cursor == ' ' || **cursor == ',') {
        (*cursor)++;
    }
    if (**cursor == '\0') {
        return 0;
    }

    size_t n = 0;
    const char* p = *cursor;
    while (*p != '\0' && *p != ',') {
        if (n + 1 < cap) {
            out[n++] = *p;
        }
        p++;
    }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t')) {
        n--;
    }
    out[n] = '\0';

    *cursor = p;
    if (**cursor == ',') {
        (*cursor)++;
    }
    return n > 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Try each configured NTP server until one answers, then adopt its correction.
///
/// The offset is published to hes_clock, which is what actually changes the
/// gateway's view of the time; the snapshot only records how we got there (which
/// is what the sourceOfTime object reports).
static void refresh_time(time_sync_t* ts)
{
    const char* cursor = ts->servers;
    char server[128];

    while (next_csv(&cursor, server, sizeof(server))) {
        sntp_result_t r;
        if (sntp_query(server, ts->cfg.timeout_ms, &r) != 0) {
            continue;
        }

        hes_clock_set_offset_ns(r.offset_ns);

        pthread_mutex_lock(&ts->lock);
        ts->snap.ntp_valid = 1;
        ts->snap.offset_ns = r.offset_ns;
        ts->snap.delay_ms = r.delay_ms;
        ts->snap.stratum = r.stratum;
        ts->snap.last_sync = time(NULL);
        ts->snap.reachable = 1;
        pthread_mutex_unlock(&ts->lock);

        log_info("synced from %s: offset %+.3f s, delay %ld ms, stratum %d", server,
                 (double)r.offset_ns / 1000000000.0, r.delay_ms, r.stratum);
        return;
    }

    log_debug("time: no NTP server answered; keeping the last correction");
}

////////////////////////////////////////////////////////////////////////////////
/// Work out which zone we are in and what its two offsets are.
///
/// Precedence: the operator's --time-zone, then a provider, then the host's own
/// idea of its zone. A zone we already learned is never replaced by the host
/// fallback, so a provider that is briefly unreachable does not flap the gateway
/// back to a stale local value.
static void refresh_zone(time_sync_t* ts)
{
    char name[GEO_TZ_NAME_MAX];
    int have_name = 0;

    if (ts->forced_tz[0]) {
        hes_strlcpy(name, sizeof(name), ts->forced_tz);
        have_name = 1;
    }

    if (!have_name && ts->cfg.tz_enabled) {
        const char* cursor = ts->tz_urls;
        char url[192];
        while (next_csv(&cursor, url, sizeof(url))) {
            if (geo_tz_detect(url, NULL, name, sizeof(name), ts->cfg.timeout_ms) != 0) {
                continue;
            }

            have_name = 1;
            pthread_mutex_lock(&ts->lock);
            ts->snap.reachable = 1;
            pthread_mutex_unlock(&ts->lock);
            break;
        }
    }

    if (!have_name) {
        pthread_mutex_lock(&ts->lock);
        int already_known = ts->snap.tz_valid;
        pthread_mutex_unlock(&ts->lock);
        if (already_known) {
            return;
        }

        if (geo_tz_system_name(name, sizeof(name)) != 0) {
            log_debug("time: no time zone known yet (no provider answer, host names none)");
            return;
        }
    }

    int std_min = 0;
    int dst_min = 0;
    if (geo_tz_offsets(name, &std_min, &dst_min) != 0) {
        return;
    }

    pthread_mutex_lock(&ts->lock);
    int changed = !ts->snap.tz_valid || strcmp(ts->snap.tz_name, name) != 0 ||
                  ts->snap.std_offset_min != std_min || ts->snap.dst_offset_min != dst_min;
    ts->snap.tz_valid = 1;
    hes_strlcpy(ts->snap.tz_name, sizeof(ts->snap.tz_name), name);
    ts->snap.std_offset_min = std_min;
    ts->snap.dst_offset_min = dst_min;
    pthread_mutex_unlock(&ts->lock);

    if (changed) {
        log_info("time: local time zone is %s (UTC%+d min, daylight %+d min)", name, std_min,
                 dst_min);
    }
}

////////////////////////////////////////////////////////////////////////////////
/// Sleep until 'deadline_mono' (milliseconds), or until stop is requested.
static void wait_until(time_sync_t* ts, long long deadline_mono)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);

    long long delta_ms =
            (deadline_mono < 0) ? (long long)IDLE_WAIT_S * 1000 : deadline_mono - monotonic_ms();
    if (delta_ms < 0) {
        delta_ms = 0;
    }

    until.tv_sec += (time_t)(delta_ms / 1000);
    until.tv_nsec += (long)(delta_ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec += 1;
        until.tv_nsec -= 1000000000L;
    }

    pthread_cond_timedwait(&ts->wake, &ts->lock, &until);
}

////////////////////////////////////////////////////////////////////////////////
/// The sync thread: resolves the zone at startup (and again on the configured
/// interval while zone lookups are enabled), and keeps the NTP offset fresh
/// while NTP is enabled.
///
/// It waits on the condition variable with a timeout, so time_sync_stop() can
/// wake it at once instead of the shutdown having to wait for the next tick.
///
/// @param arg The time_sync_t to drive.
/// @return Always NULL; the thread's exit value is unused.
static void* sync_worker(void* arg)
{
    time_sync_t* ts = (time_sync_t*)arg;

    // -1 means "never again"; the zone is always resolved once at startup so a
    // gateway with no operator setting and no Internet still knows its zone.
    long long next_ntp = ts->cfg.ntp_enabled ? 0 : -1;
    long long next_tz = 0;

    for (;;) {
        if (next_tz >= 0 && monotonic_ms() >= next_tz) {
            refresh_zone(ts);
            next_tz = ts->cfg.tz_enabled ? monotonic_ms() + ts->cfg.tz_interval_s * 1000
                                         : -1;  // one resolution is all we need
        }

        if (next_ntp >= 0 && monotonic_ms() >= next_ntp) {
            refresh_time(ts);
            next_ntp = monotonic_ms() + ts->cfg.ntp_interval_s * 1000;
        }

        long long next = -1;
        if (next_tz >= 0) {
            next = next_tz;
        }
        if (next_ntp >= 0 && (next < 0 || next_ntp < next)) {
            next = next_ntp;
        }

        pthread_mutex_lock(&ts->lock);
        if (!ts->stop) {
            wait_until(ts, next);
        }
        int stop = ts->stop;
        pthread_mutex_unlock(&ts->lock);

        if (stop) {
            break;
        }
    }

    return NULL;
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

time_sync_t* time_sync_start(const time_sync_config_t* cfg)
{
    time_sync_t* ts = calloc(1, sizeof(*ts));
    if (ts == NULL) {
        log_error("out of memory");
        return NULL;
    }

    ts->cfg = *cfg;

    hes_strlcpy(ts->servers, sizeof(ts->servers),
                (cfg->ntp_servers != NULL && *cfg->ntp_servers != '\0')
                        ? cfg->ntp_servers
                        : TIME_SYNC_DEFAULT_SERVERS);
    hes_strlcpy(
            ts->tz_urls, sizeof(ts->tz_urls),
            (cfg->tz_urls != NULL && *cfg->tz_urls != '\0') ? cfg->tz_urls : GEO_TZ_DEFAULT_URLS);
    if (cfg->tz_name != NULL) {
        hes_strlcpy(ts->forced_tz, sizeof(ts->forced_tz), cfg->tz_name);
    }

    ts->cfg.ntp_servers = ts->servers;
    ts->cfg.tz_urls = ts->tz_urls;
    ts->cfg.tz_name = ts->forced_tz;

    ts->cfg.ntp_interval_s = (cfg->ntp_interval_s >= MIN_NTP_INTERVAL_S)
                                     ? cfg->ntp_interval_s
                                     : TIME_SYNC_DEFAULT_NTP_INTERVAL_S;
    ts->cfg.tz_interval_s = (cfg->tz_interval_s >= MIN_TZ_INTERVAL_S)
                                    ? cfg->tz_interval_s
                                    : TIME_SYNC_DEFAULT_TZ_INTERVAL_S;
    ts->cfg.timeout_ms = (cfg->timeout_ms > 0) ? cfg->timeout_ms : TIME_SYNC_DEFAULT_TIMEOUT_MS;

    pthread_mutex_init(&ts->lock, NULL);
    pthread_cond_init(&ts->wake, NULL);

    if (pthread_create(&ts->thread, NULL, sync_worker, ts) != 0) {
        log_error("time: cannot start the sync thread");
        pthread_cond_destroy(&ts->wake);
        pthread_mutex_destroy(&ts->lock);
        free(ts);
        return NULL;
    }

    return ts;
}

void time_sync_get(time_sync_t* ts, time_snapshot_t* out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (ts == NULL) {
        return;
    }

    pthread_mutex_lock(&ts->lock);
    *out = ts->snap;
    pthread_mutex_unlock(&ts->lock);
}

void time_sync_stop(time_sync_t* ts)
{
    if (ts == NULL) {
        return;
    }

    pthread_mutex_lock(&ts->lock);
    ts->stop = 1;
    pthread_cond_signal(&ts->wake);
    pthread_mutex_unlock(&ts->lock);

    pthread_join(ts->thread, NULL);

    pthread_cond_destroy(&ts->wake);
    pthread_mutex_destroy(&ts->lock);
    free(ts);
}
