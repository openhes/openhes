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
/// @brief Offline test for local message routing and the two ways a client can
/// address one datum (ISO/IEC 18012-4).
///
/// @details
/// The same data can be asked for three ways, and all three must agree:
///
///   get /lx/ob/so/id              ->  the whole object   "rq=ma;si=ye"
///   get /lx/ob/so/id/rq           ->  one datum          "ma"
///   get /lx/ob/so/id?da=rq,si     ->  several data       "rq=ma;si=ye"
///
/// Two objects are registered, one nested inside the other's address, so the
/// longest-prefix rule is exercised for real:
///
///   /lx/ob/so/id                (configurationData)
///   /lx/ob/so/id/co/st/cv       (centralOperations)
///
/// Uses the REAL vendored nng: the dispatcher sends its GET answers on a pub
/// socket listening on inproc://, and a subscribed socket captures them, so the
/// message actually travels the same path as in production.
///
/// The cases are munit tests (deps/munit). Each one has a name in the output,
/// and the exit code of the process says whether all of them passed.
///
/// Build/run: see tests/CMakeLists.txt (ctest).

#include "common/hes_dispatch.h"

#include "common/hes_bus.h"
#include "common/hes_common.h"
#include "common/service_object.h"

#include <munit.h>

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAPTURE_URL "inproc://test_dispatch"

static hes_bus_t g_bus;
static nng_socket g_cap;
static int g_puts;

////////////////////////////////////////////////////////////////////////////////
// two stand-in service objects, answering the POC's payload convention
////////////////////////////////////////////////////////////////////////////////

static void config_on_get(service_object_t* so, hes_clme_msg_t* out)
{
    (void)so;
    hes_msg_set_payload_str(out, "rq=ma;si=ye");
}

static void status_on_get(service_object_t* so, hes_clme_msg_t* out)
{
    (void)so;
    hes_msg_set_payload_str(out, "pi=abc;vr=1.0.0;nh=2");
}

static void any_on_put(service_object_t* so, const hes_clme_msg_t* in)
{
    (void)so;
    (void)in;
    g_puts++;
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

////////////////////////////////////////////////////////////////////////////////
/// Sends one GET and returns the payload that came back ("" when none did).
static const char* get(const char* path, const char* query)
{
    static char payload[HES_PAYLOAD_MAX];
    static service_object_t config;
    static service_object_t status;
    service_object_t* objs[] = {&config, &status};
    hes_clme_msg_t out;
    size_t sz = sizeof(out);

    hes_strlcpy(config.path, sizeof(config.path), "/lx/ob/so/id");
    config.on_get = config_on_get;
    config.on_put = any_on_put;
    hes_strlcpy(status.path, sizeof(status.path), "/lx/ob/so/id/co/st/cv");
    status.on_get = status_on_get;
    status.on_put = any_on_put;

    hes_clme_msg_t in = {0};
    in.verb = HES_VERB_GET;
    hes_msg_set_path(&in, path);
    hes_strlcpy(in.query, sizeof(in.query), query ? query : "");

    memset(payload, 0, sizeof(payload));
    if (dispatch_to_local_objects(&g_bus, objs, 2, &in) == 0) {
        return payload;  // nothing owns this address
    }

    if (nng_recv(g_cap, &out, &sz, 0) != 0) {
        munit_errorf("no answer for %s", path);
    }
    hes_strlcpy(payload, sizeof(payload), out.payload);
    return payload;
}

////////////////////////////////////////////////////////////////////////////////
/// Fixture: one capture bus per test, so no test inherits the previous one's
/// messages.
static void* cap_setup(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    cap_start();
    return NULL;
}

static void cap_tear_down(void* fixture)
{
    (void)fixture;

    nng_close(g_cap);
    nng_close(g_bus.pub_sock);
}

/// The object's own address asks for all of its data.
static MunitResult test_whole_object(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    ////////////////////////////////////////////////////////////////////////////////
    // the object's own address: all of its data
    ////////////////////////////////////////////////////////////////////////////////
    munit_assert_string_equal(get("/lx/ob/so/id", NULL), "rq=ma;si=ye");
    return MUNIT_OK;
}

/// The data-item form: appending the transCode selects one datum.
static MunitResult test_data_item_form(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    ////////////////////////////////////////////////////////////////////////////////
    // the data-item form: append the transCode
    ////////////////////////////////////////////////////////////////////////////////
    munit_assert_string_equal(get("/lx/ob/so/id/rq", NULL), "rq=ma");
    munit_assert_string_equal(get("/lx/ob/so/id/si", NULL), "si=ye");
    return MUNIT_OK;
}

/// The 'da' query asks for several data points in one request; the answer
/// carries them in the object's own order, whatever order the client used.
static MunitResult test_da_query(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;


    ////////////////////////////////////////////////////////////////////////////////
    // the 'da' query: several data points in one request
    ////////////////////////////////////////////////////////////////////////////////
    munit_assert_string_equal(get("/lx/ob/so/id", "da=rq,si"), "rq=ma;si=ye");
    munit_assert_string_equal(get("/lx/ob/so/id", "da=si"), "si=ye");
    munit_assert_string_equal(get("/lx/ob/so/id", "da=si,rq"), "rq=ma;si=ye");
    munit_assert_string_equal(get("/lx/ob/so/id", "da= rq , si "), "rq=ma;si=ye");
    return MUNIT_OK;
}

/// Longest prefix wins: the nested object answers, not its parent.
static MunitResult test_longest_prefix(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;


    ////////////////////////////////////////////////////////////////////////////////
    // longest prefix wins: the nested object, not its parent
    ////////////////////////////////////////////////////////////////////////////////
    munit_assert_string_equal(get("/lx/ob/so/id/co/st/cv", NULL), "pi=abc;vr=1.0.0;nh=2");
    munit_assert_string_equal(get("/lx/ob/so/id/co/st/cv/pi", NULL), "pi=abc");
    munit_assert_string_equal(get("/lx/ob/so/id/co/st/cv", "da=vr,nh"), "vr=1.0.0;nh=2");
    return MUNIT_OK;
}

static MunitResult test_address_beats_query(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;


    // Using both forms at once is a client error: the address is the more
    // specific, so it wins and the query is ignored (with a diagnostic).
    munit_assert_string_equal(get("/lx/ob/so/id/co/st/cv/pi", "da=vr,nh"), "pi=abc");
    return MUNIT_OK;
}

static MunitResult test_unknown_datum(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;


    ////////////////////////////////////////////////////////////////////////////////
    // asking for something that is not there answers nothing
    ////////////////////////////////////////////////////////////////////////////////
    munit_assert_string_equal(get("/lx/ob/so/id/nope", NULL), "");
    munit_assert_string_equal(get("/lx/ob/so/id", "da=nope"), "");
    munit_assert_string_equal(get("/lx/ob/so/id", "da="), "");
    return MUNIT_OK;
}

static MunitResult test_unserved_address(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;


    ////////////////////////////////////////////////////////////////////////////////
    // a nested address we do not serve is not routed to a parent
    ////////////////////////////////////////////////////////////////////////////////
    munit_assert_string_equal(get("/lx/ob/so/id/co/st", NULL), "");
    munit_assert_string_equal(get("/lx/ob/uo/li/ll/da/cv", NULL), "");
    return MUNIT_OK;
}

static MunitResult test_put_reaches_object(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    static service_object_t config;
    service_object_t* objs[] = {&config};

    hes_strlcpy(config.path, sizeof(config.path), "/lx/ob/so/id");
    config.on_put = any_on_put;

    hes_clme_msg_t in = {0};
    in.verb = HES_VERB_PUT;
    hes_msg_set_path(&in, "/lx/ob/so/id/rq");

    g_puts = 0;
    munit_assert_int(dispatch_to_local_objects(&g_bus, objs, 1, &in), ==, 1);
    munit_assert_int(g_puts, ==, 1);
    return MUNIT_OK;
}

static MunitTest dispatch_tests[] = {
  { (char*)"/whole-object", test_whole_object, cap_setup, cap_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/data-item", test_data_item_form, cap_setup, cap_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/da-query", test_da_query, cap_setup, cap_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/longest-prefix", test_longest_prefix, cap_setup, cap_tear_down, MUNIT_TEST_OPTION_NONE,
    NULL },
  { (char*)"/address-beats-query", test_address_beats_query, cap_setup, cap_tear_down,
    MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/unknown-datum", test_unknown_datum, cap_setup, cap_tear_down, MUNIT_TEST_OPTION_NONE,
    NULL },
  { (char*)"/unserved-address", test_unserved_address, cap_setup, cap_tear_down,
    MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/put-reaches-object", test_put_reaches_object, cap_setup, cap_tear_down,
    MUNIT_TEST_OPTION_NONE, NULL },
  { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite dispatch_suite = {
  (char*)"/dispatch", dispatch_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char* argv[])
{
  return munit_suite_main(&dispatch_suite, NULL, argc, argv);
}
