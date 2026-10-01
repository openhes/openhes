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
/// @brief Offline unit test for the binding-map engine (src/bm/bm.c), exercising
/// it against the real (vendored) nng + libxml2 -- no stub.
///
/// @details
/// Why this exists / how it differs from poc2/tests:
///   - poc2 tested its own binding_map.c against a hand-rolled NNG stub that
///     recorded sends. The root tree builds against the real nng, so this test
///     instead captures the binding map's sends over a REAL inproc PUB/SUB pair:
///     we open a pub socket (handed to the binding map as its "bus"), listen on
///     inproc://, and dial a capture SUB subscribed to "" -- every message the
///     map sends is received and asserted on.
///   - The XML fixtures live in <root>/tests/data (bm_*.xml), NOT the poc2
///     sample documents, so assertions target THIS scenario.
///
/// The cases are munit tests (deps/munit): each one has a name in the output,
/// and munit owns the command line, so the fixture path is no longer an
/// argument (`test_bm --list` names the cases).
///
/// Scenario under test: tests/data/bm_button_controlled_light.xml
///   - one rule: SensorTag button (di=6) down  -> light (di=2) ON
///               SensorTag button (di=6) up    -> light (di=2) OFF
///   - controller: one SUBSCRIBE for the button object (the only external input)
///   - processor:  button=1 -> PUT light "1"; button=0 -> PUT light "0";
///                 unchanged value -> no re-send (change detection).
///
/// Build/run: see tests/CMakeLists.txt (ctest), or (one command):
///     cc -Wall -Wextra -I../src -I../deps/nng/include
///        -I../deps/libxml2/include test_bm.c ../src/bm/bm.c
///        ../src/common/hes_bus.c -lnng -lxml2 -lm -o test_bm
///     ./test_bm [path-to-bm_button_controlled_light.xml]

#include "../src/bm/bm.h"

#include <munit.h>

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTN_PATH "/lx/ob/uo/ui/ud/da/cv"
#define LIGHT_PATH "/lx/ob/uo/li/ll/da/cv"

#define MAX_CAPTURED 16

#define FIXTURE "data/bm_button_controlled_light.xml"

static hes_bus_t g_bus;   // the bus we hand to the binding map
static nng_socket g_cap;  // our capture subscriber

////////////////////////////////////////////////////////////////////////////////
/// Build an inproc pub/sub so the binding map's hes_bus_send() calls land in
/// our capture socket (real nng, no stub).
static void cap_start(void)
{
    memset(&g_bus, 0, sizeof(g_bus));

    if (nng_pub0_open(&g_bus.pub_sock) != 0 ||
        nng_listen(g_bus.pub_sock, "inproc://test_bm_cap", NULL, 0) != 0 ||
        nng_sub0_open(&g_cap) != 0 ||
        nng_socket_set(g_cap, NNG_OPT_SUB_SUBSCRIBE, "", 0) != 0 ||
        nng_dial(g_cap, "inproc://test_bm_cap", NULL, 0) != 0) {
        munit_error("cannot set up the capture bus");
    }
    nng_socket_set_ms(g_cap, NNG_OPT_RECVTIMEO, 300);
    nng_msleep(50);  // let the inproc subscription settle
}

////////////////////////////////////////////////////////////////////////////////
/// Read exactly the n messages the last action should have produced.
/// Also checks that NO extra message arrived. Returns n on success, -1 on
/// timeout / mismatch / unexpected extra.
static int expect_sends(int n, hes_clme_msg_t* out)
{
    int got = 0;
    for (int i = 0; i < n; i++) {
        size_t sz = sizeof(hes_clme_msg_t);
        int rv = nng_recv(g_cap, &out[got], &sz, 0);
        if (rv != 0) {
            fprintf(stderr, "test: timeout waiting for msg %d of %d\n", i, n);
            return -1;
        }
        if (sz != sizeof(hes_clme_msg_t)) {
            fprintf(stderr, "test: bad message size %zu\n", sz);
            return -1;
        }
        got++;
    }
    // Ensure there is no unexpected extra send.
    hes_clme_msg_t extra;
    size_t sz = sizeof(extra);
    int rv = nng_recv(g_cap, &extra, &sz, 0);
    if (rv == 0) {
        fprintf(stderr, "test: unexpected extra send: verb=%u path=%s payload=%s\n",
                (unsigned)extra.verb, extra.path, extra.payload);
        return -1;
    }
    return got;
}

////////////////////////////////////////////////////////////////////////////////
/// Feed an event into the binding-map processor.
static void feed(binding_map_t* bm, const char* path, double v)
{
    hes_clme_msg_t in = {0};
    in.verb = HES_VERB_EVENT;
    hes_msg_set_path(&in, path);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f", v);
    hes_msg_set_payload_str(&in, buf);
    bm_processor_handle(bm, &g_bus, &in);
}

////////////////////////////////////////////////////////////////////////////////
/// Same, but stamped with a source deviceIndex to exercise (path, di)
/// attribution.
static void feed_dev(binding_map_t* bm, const char* path, uint32_t di, double v)
{
    hes_clme_msg_t in = {0};
    in.verb = HES_VERB_EVENT;
    in.device_index = di;
    hes_msg_set_path(&in, path);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f", v);
    hes_msg_set_payload_str(&in, buf);
    bm_processor_handle(bm, &g_bus, &in);
}

static int is_put(const hes_clme_msg_t* m, const char* path, uint32_t di, const char* payload)
{
    return m->verb == HES_VERB_PUT && m->device_index == di && strcmp(m->path, path) == 0 &&
           strcmp(m->payload, payload) == 0;
}

/// The XML -> in-memory map: one row, two addressing rows, and the row's own
/// fields.
static MunitResult test_parse(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    binding_map_t bm;
    bm_init(&bm);
    munit_assert_int(bm_load_xml(&bm, FIXTURE), ==, 0);

    munit_assert_int(bm.n_ops, ==, 1);
    munit_assert_int(bm.n_addrs, ==, 2);

    const bm_operation_t* op = &bm.ops[0];
    munit_assert_string_equal(op->operation, "gt");
    munit_assert_true(op->enabled);
    munit_assert_int(op->n_inputs, ==, 1);
    munit_assert_uint(op->inputs[0].device_index, ==, 6);
    munit_assert_string_equal(op->inputs[0].source_object, BTN_PATH);
    munit_assert_uint(op->out_device_index, ==, 2);
    munit_assert_string_equal(op->out_dest_object, LIGHT_PATH);
    munit_assert_not_null(bm_find_addr(&bm, 6));
    munit_assert_not_null(bm_find_addr(&bm, 2));
    return MUNIT_OK;
}

/// Loads the map, starts the capture bus, and consumes the controller's one
/// SUBSCRIBE -- the state every scenario test starts from.
static void scenario_start(binding_map_t* bm)
{
    bm_init(bm);
    if (bm_load_xml(bm, FIXTURE) != 0) {
        munit_errorf("cannot load %s", FIXTURE);
    }

    cap_start();
    bm_controller_start(bm, &g_bus);

    hes_clme_msg_t msg;
    if (expect_sends(1, &msg) != 1) {
        munit_error("the controller did not send exactly one SUBSCRIBE");
    }
}

static void capture_tear_down(void* fixture)
{
    (void)fixture;

    nng_close(g_cap);
    nng_close(g_bus.pub_sock);
}

/// The controller subscribes to exactly one object: the button, the map's only
/// external input.
static MunitResult test_controller_subscribe(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    binding_map_t bm;
    bm_init(&bm);
    munit_assert_int(bm_load_xml(&bm, FIXTURE), ==, 0);

    cap_start();

    ////////////////////////////////////////////////////////////////////////////////
    // controller: exactly one SUBSCRIBE, for the button (external)
    ////////////////////////////////////////////////////////////////////////////////
    hes_clme_msg_t msgs[MAX_CAPTURED];
    bm_controller_start(&bm, &g_bus);
    munit_assert_int(expect_sends(1, msgs), ==, 1);
    munit_assert_uint(msgs[0].verb, ==, HES_VERB_SUBSCRIBE);
    munit_assert_uint(msgs[0].device_index, ==, 6);
    munit_assert_string_equal(msgs[0].path, BTN_PATH);
    return MUNIT_OK;
}

/// A question is not a reading: a GET request, and an EVENT that carries no
/// payload, must not become the input's value.
static MunitResult test_get_is_not_a_reading(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    binding_map_t bm;
    scenario_start(&bm);

    hes_clme_msg_t msgs[MAX_CAPTURED];

    // The request carries only a path, so reading a value out of it would store
    // bm_datum_value("") == 0.0 as the button's value -- a reading never taken --
    // and fire the row. Nothing may be sent, and the input must stay unknown.
    hes_clme_msg_t req = {0};
    req.verb = HES_VERB_GET;
    hes_msg_set_path(&req, BTN_PATH);
    bm_processor_handle(&bm, &g_bus, &req);
    munit_assert_int(expect_sends(0, msgs), ==, 0);

    // The same goes for an EVENT that carries no reading.
    hes_clme_msg_t ev = {0};
    ev.verb = HES_VERB_EVENT;
    hes_msg_set_path(&ev, BTN_PATH);
    bm_processor_handle(&bm, &g_bus, &ev);
    munit_assert_int(expect_sends(0, msgs), ==, 0);
    return MUNIT_OK;
}

/// The scenario, in order: press -> on, release -> off, the same value again ->
/// nothing, and press -> on again. One case, because each step depends on the
/// value the previous one left behind -- the change detection is the point.
static MunitResult test_button_toggle(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    binding_map_t bm;
    scenario_start(&bm);

    hes_clme_msg_t msgs[MAX_CAPTURED];

    // button down -> light ON
    feed_dev(&bm, BTN_PATH, 6, 1.0);
    munit_assert_int(expect_sends(1, msgs), ==, 1);
    munit_assert_true(is_put(&msgs[0], LIGHT_PATH, 2, "1"));

    // button up -> light OFF (legacy, no-di attribution path)
    feed(&bm, BTN_PATH, 0.0);
    munit_assert_int(expect_sends(1, msgs), ==, 1);
    munit_assert_true(is_put(&msgs[0], LIGHT_PATH, 2, "0"));

    // the same value again -> no re-send
    feed(&bm, BTN_PATH, 0.0);
    munit_assert_int(expect_sends(0, msgs), ==, 0);

    // and it flips again
    feed_dev(&bm, BTN_PATH, 6, 1.0);
    munit_assert_int(expect_sends(1, msgs), ==, 1);
    munit_assert_true(is_put(&msgs[0], LIGHT_PATH, 2, "1"));
    return MUNIT_OK;
}

static MunitTest bm_tests[] = {
  { (char*)"/parse", test_parse, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/controller-subscribe", test_controller_subscribe, NULL, capture_tear_down,
    MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/get-is-not-a-reading", test_get_is_not_a_reading, NULL, capture_tear_down,
    MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/button-toggle", test_button_toggle, NULL, capture_tear_down, MUNIT_TEST_OPTION_NONE,
    NULL },
  { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite bm_suite = {
  (char*)"/bm", bm_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char* argv[])
{
  return munit_suite_main(&bm_suite, NULL, argc, argv);
}
