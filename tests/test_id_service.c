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
/// @brief Offline test for the identification service (ISO/IEC 18012-3 11.2.2),
/// called directly, so no message goes over the bus.
///
/// @details
/// The test checks three things:
///
///   1. The service answers on three Lexicon addresses, one for each table
///      group. The memoryType of each table decides what the answer may hold:
///
///        /lx/ob/so/id            Table 37 configurationData ('pr') rq, si
///        /lx/ob/so/id/co/st/cv   Tables 41/42/43/44 ('ro'/'op'/'sp'/'po')
///        /lx/ac/so/id/dc/bm/cv   Table 39 discovery ('op')
///
///   2. The identity document is checked strictly. 'pi' and 'fp' are 256-bit
///      values (11.2.2.1) written as 64 hex characters. A value that is short,
///      non-hex, or missing is rejected. Nothing is padded, defaulted, or
///      generated again: 11.2.2.1 requires the public ID to be *stationary*, so
///      making a new one without telling anyone would be worse than not
///      starting.
///
///   3. The fingerprint is never given out. Table 41 gives 'pi' memoryType 'ro'
///      (viewable) and Table 43 gives 'fp' 'sp' (obscured), so the status answer
///      holds the public ID and no part of the secret.
///
/// The data files are the identity*.json documents in tests/data/. ctest runs
/// the binary from tests/, so the relative paths work.
///
/// These are munit tests (deps/munit). Each test has a name in the output, and
/// the exit code of the process says whether all of them passed.
///
/// Build/run: see tests/CMakeLists.txt (ctest), or compile with the same
/// includes/libs as test_auth_propagate.c.

#include "services/id/id.h"

#include "common/hes_common.h"
#include "common/service_object.h"

#include <munit.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_FP "9c41d2e83a7b0f5c6d1e8a9b4f203c7d5a6b1e9f0c8d3a7b2e5f4c1d9a8b7e6f"

/// How many leading characters of 'fp' must never appear anywhere. Testing a
/// prefix catches the realistic leak (a truncated or partly masked value), not
/// just an exact copy.
#define FP_HEAD_CHARS 16

////////////////////////////////////////////////////////////////////////////////
/// 'pi' is DERIVED from 'fp' now, so a data file cannot hold the expected
/// value (that is the point: it is a key, not a constant). These helpers check
/// the shape of what is served instead.
static int is_lower_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

////////////////////////////////////////////////////////////////////////////////
/// True when the answer carries 'pi=' followed by 64 lowercase hex characters.
static int has_derived_pi(const char* payload)
{
    const char* p = strstr(payload, "pi=");
    if (!p) {
        return 0;
    }

    p += 3;
    for (int i = 0; i < 64; i++) {
        if (!is_lower_hex(p[i])) {
            return 0;
        }
    }

    return p[64] == ';' || p[64] == '\0';
}

////////////////////////////////////////////////////////////////////////////////
/// Asserts that the service rejects one identity document, for any kind of
/// error: an unreadable path, malformed JSON, a missing or non-hex 'fp', or a
/// 'pi' that does not match 'fp'.
///
/// The message names the document, because munit would otherwise report only
/// this line -- and the caller below tries seven documents in a row.
static void assert_refused(const char* doc)
{
    service_object_t* bad[ID_SERVICE_OBJECT_COUNT] = {0};
    int rv = id_service_create(doc, bad, ID_SERVICE_OBJECT_COUNT);
    if (rv != -1) {
        munit_errorf("document was accepted instead of refused: '%s'", doc);
    }
}

////////////////////////////////////////////////////////////////////////////////
/// Runs a GET the way core.c does: the request fields are filled in first, then
/// on_get overwrites the payload. Returns what the service answered.
static const char* get_payload(service_object_t* so)
{
    static char buf[HES_PAYLOAD_MAX] = {0};
    hes_clme_msg_t out = {0};
    out.verb = HES_VERB_GET;
    hes_msg_set_path(&out, so->path);
    so->on_get(so, &out);

    hes_strlcpy(buf, sizeof(buf), out.payload);
    return buf;
}

////////////////////////////////////////////////////////////////////////////////
/// Stands in for core.c's core_gateway_inventory(): the host is the only one
/// that knows the gateway's module inventory and binding maps.
static void fake_inventory(void* ctx, id_inventory_t* out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    out->hans_number = 2;
    out->wans_number = 1;
    out->service_modules_number = 1;
    out->n_binding_maps = 1;
    hes_strlcpy(out->binding_maps[0].module_type, sizeof(out->binding_maps[0].module_type), "sm");
    out->binding_maps[0].module_ref_index = 1;
    out->binding_maps[0].net_ref_index = 0;
}

/// A well-formed document creates one object per address, serves the preset
/// tables, and never exposes the secret.
static MunitResult test_well_formed(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    service_object_t* so[ID_SERVICE_OBJECT_COUNT] = {0};

    ////////////////////////////////////////////////////////////////////////////////
    // a well-formed document creates one object per address
    ////////////////////////////////////////////////////////////////////////////////
    int rv = id_service_create("data/identity.json", so, ID_SERVICE_OBJECT_COUNT);
    munit_assert_int(rv, ==, ID_SERVICE_OBJECT_COUNT);
    munit_assert_string_equal(so[0]->path, HES_LX_ID);
    munit_assert_string_equal(so[1]->path, HES_LX_ID_CENTRALOPS_CV);
    munit_assert_string_equal(so[2]->path, HES_LX_ID_DISCOVER_BM);

    // Table 37: the capability declaration, 'pr' -- the standard's own
    // preassigned values, not configuration.
    munit_assert_string_equal(get_payload(so[0]), "rq=ma;si=ye");

    // Tables 41/42/44: served together from the one centralOperations address.
    // Without an inventory source the counts are honestly zero.
    const char* st = get_payload(so[1]);
    munit_assert_true(has_derived_pi(st));  // Table 41, 'ro': the public key derived from 'fp'
    munit_assert_not_null(strstr(st, "vr=1.0.0"));
    munit_assert_not_null(strstr(st, "hs=1"));
    munit_assert_not_null(strstr(st, "ch=sh"));
    munit_assert_not_null(strstr(st, "pd=Demo gateway"));
    munit_assert_not_null(strstr(st, "vl=2;il=3;ll=2;rl=6"));  // Table 44, 'po'
    munit_assert_not_null(strstr(st, "nh=0;nw=0;ns=0"));       // Table 42 before wiring

    // 'fp' is 'sp' / obscured: not the whole value, not a prefix of it, and
    // not even under its own transCode.
    char fp_head[FP_HEAD_CHARS + 1] = {0};
    memcpy(fp_head, DEMO_FP, FP_HEAD_CHARS);
    fp_head[FP_HEAD_CHARS] = '\0';

    munit_assert_null(strstr(st, DEMO_FP));
    munit_assert_null(strstr(st, fp_head));
    munit_assert_null(strstr(st, "fp="));

    // Table 39: discovery is an action ('ac'), reporting where the binding maps
    // are. With the host wired in, that is the service module hosting one.
    id_service_set_inventory_source(so[0], fake_inventory, NULL);
    munit_assert_string_equal(get_payload(so[2]), "mt=sm,mi=1,ni=0,ad=");

    // ... and the counts (Table 42) come from the same hook.
    munit_assert_not_null(strstr(get_payload(so[1]), "nh=2;nw=1;ns=1"));

    // Preset tables ('pr'/'ro'/'sp'/'po') are not writable over HES-CLME: the
    // PUT is ignored, and must not disturb the values.
    for (int i = 0; i < ID_SERVICE_OBJECT_COUNT; i++) {
        hes_clme_msg_t in = {0};
        in.verb = HES_VERB_PUT;
        hes_msg_set_path(&in, so[i]->path);
        hes_msg_set_payload_str(&in, "pi=0000");
        so[i]->on_put(so[i], &in);
    }
    munit_assert_true(has_derived_pi(get_payload(so[1])));

    for (int i = 0; i < ID_SERVICE_OBJECT_COUNT; i++) {
        so[i]->destroy(so[i]);
        free(so[i]);
    }
    return MUNIT_OK;
}

/// Every malformed identity document is rejected.
static MunitResult test_provisioning_failures(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    service_object_t* bad[ID_SERVICE_OBJECT_COUNT] = {0};

    munit_assert_int(id_service_create(NULL, bad, ID_SERVICE_OBJECT_COUNT), ==, -1);
    assert_refused("");
    assert_refused("data/no_such_identity.json");
    assert_refused("data/identity_bad_short.json");
    assert_refused("data/identity_bad_hex.json");
    assert_refused("data/identity_no_fp.json");
    // 'pi' is optional, but a wrong one would advertise a key that does not
    // match 'fp' -- every challenge would fail with no visible reason -- so it
    // is a startup failure rather than a warning.
    assert_refused("data/identity_wrong_pi.json");

    int rv = id_service_create("data/identity.json", bad, ID_SERVICE_OBJECT_COUNT - 1);
    munit_assert_int(rv, ==, -1);
    return MUNIT_OK;
}

static MunitTest id_service_tests[] = {
  { (char*)"/well-formed", test_well_formed, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/provisioning-failures", test_provisioning_failures, NULL, NULL,
    MUNIT_TEST_OPTION_NONE, NULL },
  { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite id_service_suite = {
  (char*)"/id-service", id_service_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char* argv[])
{
  return munit_suite_main(&id_service_suite, NULL, argc, argv);
}
