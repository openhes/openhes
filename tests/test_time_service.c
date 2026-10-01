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
/// @brief Offline test for the time service (ISO/IEC 18012-3 11.2.3) and for the
/// binding map's ability to read its answers.
///
/// @details
/// Three things are checked, all without a network:
///
///   1. The three functional objects exist at the three addresses of Tables 45,
///      49 and 53, and each answers with every transCode of the tables that
///      share its address (Tables 46-48, 50-52, 54-56).
///   2. A datum can be singled out of those answers -- both by appending its
///      transCode to the address and with '?da=' -- through the real router.
///      This is what used to fail: the old object answered a bare number, so
///      ".../cv/va" matched nothing and came back empty.
///   3. The binding map reads the value of the 'va' datum out of such a payload,
///      which is what makes a time-based rule work.
///
/// The sync state is supplied by a stub, so the test never touches the network
/// and the zone/offset values are exactly known.
///
/// Build/run: see tests/CMakeLists.txt (ctest).

#include "bm/bm.h"
#include "common/hes_bus.h"
#include "common/hes_common.h"
#include "common/hes_dispatch.h"
#include "common/service_object.h"
#include "services/time/time.h"

#include <munit.h>

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CAPTURE_URL "inproc://test_time_service"

static hes_bus_t g_bus;
static nng_socket g_cap;
static time_snapshot_t g_snap;

////////////////////////////////////////////////////////////////////////////////
/// The sync state the objects see: whatever the test last put in g_snap.
static void stub_snapshot(void* ctx, time_snapshot_t* out)
{
    (void)ctx;
    *out = g_snap;
}

static void cap_start(void)
{
    memset(&g_bus, 0, sizeof(g_bus));

    if (nng_pub0_open(&g_bus.pub_sock) != 0 ||
        nng_listen(g_bus.pub_sock, CAPTURE_URL, NULL, 0) != 0 || nng_sub0_open(&g_cap) != 0 ||
        nng_socket_set(g_cap, NNG_OPT_SUB_SUBSCRIBE, "", 0) != 0 ||
        nng_dial(g_cap, CAPTURE_URL, NULL, 0) != 0) {
        munit_error("cannot set up the capture bus");
    }
    nng_socket_set_ms(g_cap, NNG_OPT_RECVTIMEO, 300);
    nng_msleep(50);
}

static void capture_tear_down(void* fixture)
{
    (void)fixture;

    nng_close(g_cap);
    hes_bus_close(&g_bus);
}

////////////////////////////////////////////////////////////////////////////////
/// The value of one datum in a 'transCode=value;...' answer, or NULL when the
/// answer does not carry it. Matching is per record, so 'pr' is never found
/// inside 'pr=100' by a search for 'r'.
static const char* datum_value(const char* payload, const char* code)
{
    static char value[64];
    size_t clen = strlen(code);
    const char* p = payload;

    while (*p != '\0') {
        const char* end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        const char* eq = memchr(p, '=', len);

        if (eq != NULL && (size_t)(eq - p) == clen && strncmp(p, code, clen) == 0) {
            size_t vlen = len - clen - 1;
            if (vlen >= sizeof(value)) {
                vlen = sizeof(value) - 1;
            }
            memcpy(value, eq + 1, vlen);
            value[vlen] = '\0';
            return value;
        }

        if (end == NULL) {
            break;
        }
        p = end + 1;
    }
    return NULL;
}

////////////////////////////////////////////////////////////////////////////////
/// True when the datum exists and holds exactly 'want'.
static int datum_is(const char* payload, const char* code, const char* want)
{
    const char* v = datum_value(payload, code);
    return v != NULL && strcmp(v, want) == 0;
}

////////////////////////////////////////////////////////////////////////////////
// 1. the three addresses and their tables
////////////////////////////////////////////////////////////////////////////////

static void check_realtime(service_object_t* so)
{
    time_t before = time(NULL);
    hes_clme_msg_t out = {0};
    so->on_get(so, &out);
    time_t after = time(NULL);

    const char* p = out.payload;

    // Table 46, the pre-market configurationData ('ro').
    munit_assert_true(datum_is(p, "vr", "1.0.0"));
    munit_assert_true(datum_is(p, "dt", "ss"));  // secondsSinceEpoch
    munit_assert_true(datum_is(p, "ne", "1"));
    munit_assert_true(datum_is(p, "um", "se"));  // seconds
    munit_assert_true(datum_is(p, "mp", "me"));  // measured
    munit_assert_true(datum_is(p, "rq", "op"));  // optional
    munit_assert_true(datum_is(p, "pt", "tm"));  // time
    munit_assert_true(datum_is(p, "ac", "in"));
    munit_assert_not_null(datum_value(p, "sz"));
    munit_assert_not_null(datum_value(p, "sc"));
    munit_assert_not_null(datum_value(p, "cd"));
    munit_assert_not_null(datum_value(p, "cp"));

    // Table 47, the post-market configurationData ('po').
    munit_assert_true(datum_is(p, "mn", "0"));
    munit_assert_true(datum_is(p, "mx", "4294967295"));
    munit_assert_true(datum_is(p, "vl", "0"));
    munit_assert_true(datum_is(p, "rl", "0"));

    // Table 48, the interactiveData ('ra'): the corrected clock, inside the
    // range Table 47 declares.
    const char* va = datum_value(p, "va");
    munit_assert_not_null(va);
    long long epoch = atoll(va);
    munit_assert(epoch >= (long long)before && epoch <= (long long)after);
}

static void check_localtz(service_object_t* so)
{
    // Table 52 'va' is "<UTC offset minutes>,<daylight minutes>".
    g_snap.tz_valid = 1;
    g_snap.std_offset_min = -240;
    g_snap.dst_offset_min = 60;

    hes_clme_msg_t out = {0};
    so->on_get(so, &out);

    const char* p = out.payload;
    munit_assert_true(datum_is(p, "dt", "ai,ai"));  // two integers
    munit_assert_true(datum_is(p, "ne", "2"));
    munit_assert_true(datum_is(p, "um", "mi,mi"));  // minutes, minutes
    munit_assert_true(datum_is(p, "mp", "pr"));     // preset, not measured
    munit_assert_true(datum_is(p, "df", "0,0"));    // Table 51 default
    munit_assert_true(datum_is(p, "va", "-240,60"));

    // Before the zone is known, Table 51's own default is reported rather than
    // a made-up offset.
    g_snap.tz_valid = 0;
    hes_msg_init(&out);
    so->on_get(so, &out);
    munit_assert_true(datum_is(out.payload, "va", "0,0"));
}

static void check_source(service_object_t* so)
{
    // Table 56 'va' names where the time comes from.
    g_snap.ntp_valid = 1;

    hes_clme_msg_t out = {0};
    so->on_get(so, &out);
    munit_assert_true(datum_is(out.payload, "dt", "sl"));
    munit_assert_true(datum_is(out.payload, "ne", "1"));
    munit_assert_true(datum_is(out.payload, "df", "lx"));
    munit_assert_true(datum_is(out.payload, "va", "nt"));  // internet, NTP

    // Never synced: the local crystal, which is also Table 55's default.
    g_snap.ntp_valid = 0;
    hes_msg_init(&out);
    so->on_get(so, &out);
    munit_assert_true(datum_is(out.payload, "va", "lx"));
}

////////////////////////////////////////////////////////////////////////////////
// 2. reaching one datum through the real router
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// Sends one GET through the router and returns the captured payload.
static const char* routed_get(service_object_t** objs, int n, const char* path, const char* query)
{
    static char payload[HES_PAYLOAD_MAX];
    hes_clme_msg_t out;
    size_t sz = sizeof(out);

    hes_clme_msg_t in = {0};
    in.verb = HES_VERB_GET;
    hes_msg_set_path(&in, path);
    if (query != NULL) {
        hes_strlcpy(in.query, sizeof(in.query), query);
    }

    memset(payload, 0, sizeof(payload));
    if (dispatch_to_local_objects(&g_bus, objs, n, &in) == 0) {
        return payload;  // nobody owns this address
    }

    if (nng_recv(g_cap, &out, &sz, 0) != 0) {
        munit_errorf("no answer for %s", path);
    }
    hes_strlcpy(payload, sizeof(payload), out.payload);
    return payload;
}

////////////////////////////////////////////////////////////////////////////////
// 3. the binding map reads the 'va' datum
////////////////////////////////////////////////////////////////////////////////

static MunitResult test_bm_reads_datums(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    // A bare payload is the value itself: this is how the interface modules
    // report a sensor, and it must keep working.
    munit_assert_true(fabs(bm_datum_value("23.5", "va") - 23.5) < 1e-9);
    munit_assert_true(fabs(bm_datum_value("1", "va") - 1.0) < 1e-9);
    munit_assert_true(fabs(bm_datum_value("0", "va") - 0.0) < 1e-9);

    // A table answer: the 'va' record is the value, not the first record.
    munit_assert_true(fabs(bm_datum_value("vr=1.0.0;va=1785612345", "va") - 1785612345.0) < 1e-9);
    munit_assert_true(fabs(bm_datum_value("va=1785612345;vr=1.0.0", "va") - 1785612345.0) < 1e-9);

    // A table answer that does not carry 'va' must not hand back 'vr'.
    munit_assert_true(fabs(bm_datum_value("vr=1.0.0;dt=ss", "va") - 0.0) < 1e-9);
    munit_assert_true(fabs(bm_datum_value("", "va") - 0.0) < 1e-9);
    return MUNIT_OK;
}

////////////////////////////////////////////////////////////////////////////////
/// Creates the three time objects; the sync state they see comes from the stub,
/// so nothing here touches the network.
static void objects_start(service_object_t* objs[TIME_SERVICE_OBJECT_COUNT])
{
    time_service_config_t cfg;
    cfg.snapshot = stub_snapshot;
    cfg.snapshot_ctx = NULL;

    memset(&g_snap, 0, sizeof(g_snap));
    if (time_service_create(&cfg, objs, TIME_SERVICE_OBJECT_COUNT) != TIME_SERVICE_OBJECT_COUNT) {
        munit_error("cannot create the time service");
    }
}

static void objects_stop(service_object_t* objs[TIME_SERVICE_OBJECT_COUNT])
{
    for (int i = 0; i < TIME_SERVICE_OBJECT_COUNT; i++) {
        objs[i]->destroy(objs[i]);
        free(objs[i]);
    }
}

/// The three addresses of Tables 45-56, and the one autonomous object.
static MunitResult test_addresses(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    service_object_t* objs[TIME_SERVICE_OBJECT_COUNT];
    objects_start(objs);

    munit_assert_string_equal(objs[TIME_OBJECT_REALTIME]->path, "/lx/ob/so/ti/rt/st/cv");
    munit_assert_string_equal(objs[TIME_OBJECT_LOCALTZ]->path, "/lx/ob/so/ti/tz/st/cv");
    munit_assert_string_equal(objs[TIME_OBJECT_SOURCE]->path, "/lx/ob/so/ti/st/mp/cv");

    // Only realTime ticks, so only it carries the autonomous behaviour.
    munit_assert_not_null(objs[TIME_OBJECT_REALTIME]->tick);

    objects_stop(objs);
    return MUNIT_OK;
}

/// Table 46/47/48: the realTime object's own tables and its corrected clock.
static MunitResult test_realtime(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    service_object_t* objs[TIME_SERVICE_OBJECT_COUNT];
    objects_start(objs);
    check_realtime(objs[TIME_OBJECT_REALTIME]);
    objects_stop(objs);
    return MUNIT_OK;
}

/// Table 51/52: the UTC and daylight offsets, and the default before the zone
/// is known.
static MunitResult test_localtz(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    service_object_t* objs[TIME_SERVICE_OBJECT_COUNT];
    objects_start(objs);
    check_localtz(objs[TIME_OBJECT_LOCALTZ]);
    objects_stop(objs);
    return MUNIT_OK;
}

/// Table 55/56: where the time comes from, synced or not.
static MunitResult test_source(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    service_object_t* objs[TIME_SERVICE_OBJECT_COUNT];
    objects_start(objs);
    check_source(objs[TIME_OBJECT_SOURCE]);
    objects_stop(objs);
    return MUNIT_OK;
}

/// A PUT is ignored (18012-4 5.2.9.3.4: configurationData is not
/// client-updatable) -- and it must not disturb the reported clock.
static MunitResult test_put_ignored(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    service_object_t* objs[TIME_SERVICE_OBJECT_COUNT];
    objects_start(objs);

    hes_clme_msg_t put = {0};
    put.verb = HES_VERB_PUT;
    hes_msg_set_path(&put, HES_LX_TIME_REALTIME_CV);
    hes_msg_set_payload_str(&put, "va=0");
    objs[TIME_OBJECT_REALTIME]->on_put(objs[TIME_OBJECT_REALTIME], &put);

    hes_clme_msg_t after = {0};
    objs[TIME_OBJECT_REALTIME]->on_get(objs[TIME_OBJECT_REALTIME], &after);
    munit_assert(atoll(datum_value(after.payload, "va")) > 1000000000LL);

    objects_stop(objs);
    return MUNIT_OK;
}

/// One datum, two ways, through the real router: the address form and 'da='.
static MunitResult test_routed_datum(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    service_object_t* objs[TIME_SERVICE_OBJECT_COUNT];
    objects_start(objs);
    cap_start();

    g_snap.tz_valid = 1;
    g_snap.std_offset_min = -240;
    g_snap.dst_offset_min = 60;
    g_snap.ntp_valid = 1;

    const char* whole = routed_get(objs, TIME_SERVICE_OBJECT_COUNT, HES_LX_TIME_REALTIME_CV, NULL);
    munit_assert_not_null(datum_value(whole, "vr"));
    munit_assert_not_null(datum_value(whole, "va"));

    // Address the datum directly ...
    const char* one =
            routed_get(objs, TIME_SERVICE_OBJECT_COUNT, HES_LX_TIME_REALTIME_CV "/va", NULL);
    munit_assert_int(strncmp(one, "va=", 3), ==, 0);

    // ... or ask for it with 'da'. The answer keeps the object's own order
    // ('mx' is defined before 'va' in Table 47/48), not the client's.
    const char* two =
            routed_get(objs, TIME_SERVICE_OBJECT_COUNT, HES_LX_TIME_REALTIME_CV, "da=va,mx");
    munit_assert_int(strncmp(two, "mx=4294967295;va=", 17), ==, 0);

    // A datum the object does not have is empty, never a full dump.
    const char* none =
            routed_get(objs, TIME_SERVICE_OBJECT_COUNT, HES_LX_TIME_REALTIME_CV, "da=zz");
    munit_assert_char(none[0], ==, '\0');

    // The zone object works the same way.
    const char* tz_va =
            routed_get(objs, TIME_SERVICE_OBJECT_COUNT, HES_LX_TIME_LOCALTZ_CV "/va", NULL);
    munit_assert_string_equal(tz_va, "va=-240,60");

    objects_stop(objs);
    return MUNIT_OK;
}

/// The autonomous event-reports a tick publishes.
static MunitResult test_event_reports(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    service_object_t* objs[TIME_SERVICE_OBJECT_COUNT];
    objects_start(objs);
    cap_start();

    g_snap.tz_valid = 1;
    g_snap.std_offset_min = -240;
    g_snap.dst_offset_min = 60;
    g_snap.ntp_valid = 1;

    objs[TIME_OBJECT_REALTIME]->tick(objs[TIME_OBJECT_REALTIME], &g_bus);

    int saw_realtime = 0;
    int saw_source = 0;
    for (int i = 0; i < 3; i++) {
        hes_clme_msg_t ev;
        size_t sz = sizeof(ev);
        if (nng_recv(g_cap, &ev, &sz, 0) != 0) {
            break;
        }
        munit_assert_uint(ev.verb, ==, HES_VERB_EVENT);
        munit_assert_string_equal(ev.query, "va");
        if (strcmp(ev.path, HES_LX_TIME_REALTIME_CV) == 0) {
            munit_assert_not_null(datum_value(ev.payload, "va"));
            saw_realtime = 1;
        }
        if (strcmp(ev.path, HES_LX_TIME_SOURCE_CV) == 0) {
            munit_assert_true(datum_is(ev.payload, "va", "nt"));
            saw_source = 1;
        }
    }
    munit_assert_true(saw_realtime);
    munit_assert_true(saw_source);

    objects_stop(objs);
    return MUNIT_OK;
}

static MunitTest time_service_tests[] = {
  { (char*)"/addresses", test_addresses, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/realtime", test_realtime, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/localtz", test_localtz, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/source", test_source, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/put-ignored", test_put_ignored, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/routed-datum", test_routed_datum, NULL, capture_tear_down, MUNIT_TEST_OPTION_NONE,
    NULL },
  { (char*)"/event-reports", test_event_reports, NULL, capture_tear_down, MUNIT_TEST_OPTION_NONE,
    NULL },
  { (char*)"/bm-datums", test_bm_reads_datums, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite time_service_suite = {
  (char*)"/time-service", time_service_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char* argv[])
{
  return munit_suite_main(&time_service_suite, NULL, argc, argv);
}
