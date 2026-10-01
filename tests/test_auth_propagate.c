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
/// @brief Offline test for the authorization service's propagation into the
/// binding map (ISO/IEC 18012-3 11.2.4).
///
/// @details
/// What is under test:
///
///   The authorization service does not inspect traffic. It writes each
///   applicable permission's valueNew ('vn') into the binding map's authorType
///   ('at') field named by that permission's lexiconObject ('lo'). The binding
///   map then enforces 'at' locally: 'bk' suppresses the outgoing PUT.
///
/// Fixtures:
///   tests/data/bm_auth_two_targets.xml  -- two rows, both authorType 'bk' by default
///                                     (deny by default); addresses
///                                     /lx/ob/bm/ot1/op1/at and .../ot2/op1/at
///   tests/data/auth_policy_kid.json     -- kid active (st=au): opens target A,
///                                     leaves target B blocked (Annex D's
///                                     "kid partial control")
///   tests/data/auth_policy_parent.json  -- parent active (st=au): opens both
///
/// Real nng + libxml2 + jansson; no Lua, no bus round-trip needed.
/// Build/run: see tests/CMakeLists.txt (ctest).

#include "../src/bm/bm.h"
#include "../src/services/auth/auth.h"

#include <munit.h>

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTN_PATH "/lx/ob/uo/ui/ud/da/cv"
#define TARGET_A "/lx/ob/uo/li/ll/da/cv"  ///< light power, di=2, table ot1
#define TARGET_B "/lx/ob/uo/li/ll/aa/cv"  ///< light brightness, di=2, table ot2

#define MAX_CAPTURED 8

static hes_bus_t g_bus;
static nng_socket g_cap;

////////////////////////////////////////////////////////////////////////////////
// capture plumbing (same approach as the other binding-map tests)
////////////////////////////////////////////////////////////////////////////////

static void cap_start(void)
{
    memset(&g_bus, 0, sizeof(g_bus));

    if (nng_pub0_open(&g_bus.pub_sock) != 0 ||
        nng_listen(g_bus.pub_sock, "inproc://test_auth", NULL, 0) != 0 ||
        nng_sub0_open(&g_cap) != 0 ||
        nng_socket_set(g_cap, NNG_OPT_SUB_SUBSCRIBE, "", 0) != 0 ||
        nng_dial(g_cap, "inproc://test_auth", NULL, 0) != 0) {
        munit_error("cannot set up the capture bus");
    }
    nng_socket_set_ms(g_cap, NNG_OPT_RECVTIMEO, 300);
    nng_msleep(50);
}

static void capture_tear_down(void* fixture)
{
    (void)fixture;

    nng_close(g_cap);
    nng_close(g_bus.pub_sock);
}

static int expect_sends(int n, hes_clme_msg_t* out)
{
    int got = 0;
    for (int i = 0; i < n; i++) {
        size_t sz = sizeof(hes_clme_msg_t);
        if (nng_recv(g_cap, &out[got], &sz, 0) != 0) {
            fprintf(stderr, "test: timeout waiting for msg %d of %d\n", i, n);
            return -1;
        }
        got++;
    }
    hes_clme_msg_t extra;
    size_t sz = sizeof(extra);
    if (nng_recv(g_cap, &extra, &sz, 0) == 0) {
        fprintf(stderr, "test: unexpected extra send: path=%s payload=%s\n", extra.path,
                extra.payload);
        return -1;
    }
    return got;
}

static void feed(binding_map_t* bm, const char* path, uint32_t di, double v)
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

////////////////////////////////////////////////////////////////////////////////
/// The host-side writer: how the authorization service reaches the binding map.
/// Same process here, so it is a direct call (see core.c).
static int write_at(void* ctx, const char* at_address, const char* value)
{
    return bm_set_at((binding_map_t*)ctx, at_address, value);
}

static int is_put(const hes_clme_msg_t* m, const char* path, uint32_t di, const char* payload)
{
    return m->verb == HES_VERB_PUT && m->device_index == di && strcmp(m->path, path) == 0 &&
           strcmp(m->payload, payload) == 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Both rows start blocked: deny by default, before any authorization.
static MunitResult test_starts_denied(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    binding_map_t bm;
    bm_init(&bm);
    munit_assert_int(bm_load_xml(&bm, "data/bm_auth_two_targets.xml"), ==, 0);
    munit_assert_int(bm.n_ops, ==, 2);
    munit_assert_string_equal(bm.ops[0].out_author_type, "bk");
    munit_assert_string_equal(bm.ops[1].out_author_type, "bk");

    // The row addresses the authorization policy must name.
    char addr[HES_PATH_MAX];
    bm_at_address(&bm.ops[0], addr, sizeof(addr));
    munit_assert_string_equal(addr, "/lx/ob/bm/ot1/op1/at");
    bm_at_address(&bm.ops[1], addr, sizeof(addr));
    munit_assert_string_equal(addr, "/lx/ob/bm/ot2/op1/at");

    // Nothing flows while both are blocked.
    cap_start();
    hes_clme_msg_t msgs[MAX_CAPTURED];
    feed(&bm, BTN_PATH, 6, 1.0);
    munit_assert_int(expect_sends(0, msgs), ==, 0);
    feed(&bm, BTN_PATH, 7, 1.0);
    munit_assert_int(expect_sends(0, msgs), ==, 0);
    return MUNIT_OK;
}

////////////////////////////////////////////////////////////////////////////////
/// Kid active: target A opens, target B stays blocked.
static MunitResult test_kid_partial(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    binding_map_t bm;
    bm_init(&bm);
    munit_assert_int(bm_load_xml(&bm, "data/bm_auth_two_targets.xml"), ==, 0);

    service_object_t* auth = auth_service_create("data/auth_policy_kid.json");
    munit_assert_not_null(auth);

    // A GET must expose class info but never a credential.
    hes_clme_msg_t reply = {0};
    auth->on_get(auth, &reply);
    munit_assert_not_null(strstr(reply.payload, "kid"));
    munit_assert_null(strstr(reply.payload, "secret"));
    munit_assert_null(strstr(reply.payload, "parent-secret"));

    int writes = auth_service_propagate(auth, write_at, &bm);
    munit_assert_int(writes, ==, 2);  // kid's two permission rows (statusCheck 'au')

    munit_assert_string_equal(bm.ops[0].out_author_type, "fl");  // target A opened
    munit_assert_string_equal(bm.ops[1].out_author_type, "bk");  // target B blocked

    cap_start();
    hes_clme_msg_t msgs[MAX_CAPTURED];

    // Target A flows.
    feed(&bm, BTN_PATH, 6, 1.0);
    munit_assert_int(expect_sends(1, msgs), ==, 1);
    munit_assert_true(is_put(&msgs[0], TARGET_A, 2, "1"));

    // Target B is suppressed.
    feed(&bm, BTN_PATH, 7, 1.0);
    munit_assert_int(expect_sends(0, msgs), ==, 0);

    auth->destroy(auth);
    free(auth);
    return MUNIT_OK;
}

////////////////////////////////////////////////////////////////////////////////
/// Same permission tables, only the class statuses swapped: both open.
static MunitResult test_parent_full(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    binding_map_t bm;
    bm_init(&bm);
    munit_assert_int(bm_load_xml(&bm, "data/bm_auth_two_targets.xml"), ==, 0);

    service_object_t* auth = auth_service_create("data/auth_policy_parent.json");
    munit_assert_not_null(auth);

    int writes = auth_service_propagate(auth, write_at, &bm);
    munit_assert_int(writes, ==, 2);  // parent's two rows

    munit_assert_string_equal(bm.ops[0].out_author_type, "fl");
    munit_assert_string_equal(bm.ops[1].out_author_type, "fl");

    cap_start();
    hes_clme_msg_t msgs[MAX_CAPTURED];

    feed(&bm, BTN_PATH, 6, 1.0);
    munit_assert_int(expect_sends(1, msgs), ==, 1);
    munit_assert_true(is_put(&msgs[0], TARGET_A, 2, "1"));

    feed(&bm, BTN_PATH, 7, 1.0);
    munit_assert_int(expect_sends(1, msgs), ==, 1);
    munit_assert_true(is_put(&msgs[0], TARGET_B, 2, "1"));

    auth->destroy(auth);
    free(auth);
    return MUNIT_OK;
}

////////////////////////////////////////////////////////////////////////////////
/// A permission naming a binding-map address that does not exist is reported,
/// not silently ignored.
static MunitResult test_unknown_address(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    binding_map_t bm;
    bm_init(&bm);
    munit_assert_int(bm_load_xml(&bm, "data/bm_auth_two_targets.xml"), ==, 0);

    munit_assert_int(bm_set_at(&bm, "/lx/ob/bm/ot9/op1/at", "fl"), ==, -1);
    munit_assert_int(bm_set_at(&bm, "/lx/ob/bm/ot1/op1/at", "fl"), ==, 0);
    munit_assert_string_equal(bm.ops[0].out_author_type, "fl");
    return MUNIT_OK;
}

////////////////////////////////////////////////////////////////////////////////
/// The development switch (core's --no-authz): a row the policy left blocked
/// still flows once authorization is disabled -- and the 'at' field is *not*
/// rewritten, so the switch is honoured at the gate, not by editing the map.
static MunitResult test_authz_disabled(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    binding_map_t bm;
    bm_init(&bm);
    munit_assert_int(bm_load_xml(&bm, "data/bm_auth_two_targets.xml"), ==, 0);

    service_object_t* auth = auth_service_create("data/auth_policy_kid.json");
    munit_assert_not_null(auth);

    // Kid active: target A opens, target B is left at 'bk'.
    int writes = auth_service_propagate(auth, write_at, &bm);
    munit_assert_int(writes, ==, 2);
    munit_assert_string_equal(bm.ops[1].out_author_type, "bk");

    cap_start();
    hes_clme_msg_t msgs[MAX_CAPTURED];

    // Baseline: the blocked row really is suppressed.
    feed(&bm, BTN_PATH, 7, 1.0);
    munit_assert_int(expect_sends(0, msgs), ==, 0);

    // Disable authorization, then feed a *different* value: the operand cache
    // recorded the refused press (only the output is left untraced), so the same
    // value again would be skipped as "no change" rather than gated.
    bm_disable_authorization(&bm);
    munit_assert_string_equal(bm.ops[1].out_author_type, "bk");  // ignored, not rewritten
    feed(&bm, BTN_PATH, 7, 0.0);
    munit_assert_int(expect_sends(1, msgs), ==, 1);
    munit_assert_true(is_put(&msgs[0], TARGET_B, 2, "0"));

    auth->destroy(auth);
    free(auth);
    return MUNIT_OK;
}

static MunitTest auth_propagate_tests[] = {
  { (char*)"/starts-denied", test_starts_denied, NULL, capture_tear_down, MUNIT_TEST_OPTION_NONE,
    NULL },
  { (char*)"/kid-partial", test_kid_partial, NULL, capture_tear_down, MUNIT_TEST_OPTION_NONE,
    NULL },
  { (char*)"/parent-full", test_parent_full, NULL, capture_tear_down, MUNIT_TEST_OPTION_NONE,
    NULL },
  { (char*)"/unknown-address", test_unknown_address, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/authz-disabled", test_authz_disabled, NULL, capture_tear_down, MUNIT_TEST_OPTION_NONE,
    NULL },
  { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite auth_propagate_suite = {
  (char*)"/auth-propagate", auth_propagate_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char* argv[])
{
  return munit_suite_main(&auth_propagate_suite, NULL, argc, argv);
}
