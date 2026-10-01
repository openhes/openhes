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
/// @brief Offline test for per-message (Mode B) authorization: a binding-map row
/// whose input declares ap='au' only outputs when the credential travelling with
/// that message is authorized for the row's target.
///
/// @details
/// Scenario: tests/data/bm_auth_two_buttons.xml + tests/data/auth_policy_two_buttons.json
///
///   Two buttons both drive the SAME light, so the row's authorType ('at') --
///   a property of the row -- cannot distinguish the senders. Instead each
///   message carries a credential and the decision is made per message.
///
///   The policy grants 'parent' a permission for button 1's row
///   (/lx/ob/bm/ot1/op1/at) and nothing for button 2's row
///   (/lx/ob/bm/ot2/op1/at), so:
///
///     button 1 + valid credential   -> PUT to the light      (allowed)
///     button 2 + valid credential   -> suppressed            (no permission)
///     button 1 + wrong credential   -> suppressed            (unknown credential)
///
///   The last case is the point worth noting: a *valid* credential is not enough.
///   Authorization is per target, not merely "is this password correct".
///
/// Phase 2 adds the PROOF gate (tests/data/auth_policy_proof.json). The policy can mark
/// a target as needing proof of the sender's identity, and then the message's 'ci'
/// must also carry "pi=..;pk=..;sig=.." over the message's path and payload. Both
/// gates must pass. Note the ordering of the cases below: every denial uses a
/// payload the destination has NOT already got, so a suppressed send can only be
/// the proof gate talking -- never the binding map's own change detection. A test
/// that got that wrong would pass for the wrong reason.
///
/// Build/run: see tests/CMakeLists.txt (ctest), or compile with the same
/// includes/libs as test_bm.c.

#include "../src/bm/bm.h"
#include "../src/services/auth/auth.h"
#include "../src/services/crypto/crypto.h"
#include "../src/services/id/id.h"

#include <munit.h>

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTN_PATH "/lx/ob/uo/ui/ud/da/cv"
#define LIGHT_PATH "/lx/ob/uo/li/ll/da/cv"

#define BTN1_DI 6
#define BTN2_DI 7
#define LIGHT_DI 2

#define CRED_OK "parent-key"
#define CRED_BAD "not-the-key"

/// The identity the proof policy trusts: the public ID derived from the demo
/// 'fp' in identity.json (see tests/test_crypto_identity.c).
#define PEER_IDENTITY "data/identity.json"
#define OTHER_PI "0000000000000000000000000000000000000000000000000000000000000000"

#define MAX_CAPTURED 16

static hes_bus_t g_bus;
static nng_socket g_cap;

////////////////////////////////////////////////////////////////////////////////
// capture plumbing (same approach as test_bm.c)
////////////////////////////////////////////////////////////////////////////////

static void cap_start(void)
{
    memset(&g_bus, 0, sizeof(g_bus));

    if (nng_pub0_open(&g_bus.pub_sock) != 0 ||
        nng_listen(g_bus.pub_sock, "inproc://test_auth_authorize", NULL, 0) != 0 ||
        nng_sub0_open(&g_cap) != 0 ||
        nng_socket_set(g_cap, NNG_OPT_SUB_SUBSCRIBE, "", 0) != 0 ||
        nng_dial(g_cap, "inproc://test_auth_authorize", NULL, 0) != 0) {
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

////////////////////////////////////////////////////////////////////////////////
/// A bridge that stands in for core.c's wiring: bm -> [A&A policy] -> [proof].
typedef struct authorizer {
    service_object_t* auth;
    service_object_t* crypto;
} authorizer_t;

static int bridge_authorize(void* ctx, const hes_clme_msg_t* msg, const char* at_address)
{
    const authorizer_t* authz = (const authorizer_t*)ctx;

    if (!auth_service_authorize(authz->auth, msg->cred_info, msg->user_name, at_address)) {
        return 0;
    }

    const char* expected_pi = auth_service_required_proof_pi(authz->auth, at_address);
    if (!expected_pi) {
        return 1;
    }

    char framed[HES_PATH_MAX + HES_PAYLOAD_MAX + 2];
    size_t n = crypto_proof_message(msg->path, msg->payload, framed, sizeof(framed));
    return crypto_verify_proof(msg->cred_info, expected_pi, (const unsigned char*)framed, n) == 1;
}

static void to_hex(const unsigned char* data, int len, char* out)
{
    static const char DIGITS[] = "0123456789abcdef";
    for (int i = 0; i < len; i++) {
        out[2 * i] = DIGITS[data[i] >> 4];
        out[2 * i + 1] = DIGITS[data[i] & 0x0f];
    }
    out[2 * len] = '\0';
}

////////////////////////////////////////////////////////////////////////////////
/// Builds the 'ci' a proof-carrying sender would send: the credential first,
/// then the proof, ';'-separated.
///
/// @param signer The sender's key. Note that the signer and the verifier are
///               different roles here -- the gateway never signs an inbound
///               proof; this key stands in for the peer's.
/// @param sign_path The path the signature is made over, which is normally the
///               message's own path but can be deliberately wrong, to show that
///               a proof cannot be moved onto another message.
static void build_cred(char* out,
                       size_t cap,
                       const char* cred,
                       const char* sign_path,
                       const char* sign_payload,
                       service_object_t* signer,
                       int tamper)
{
    char framed[HES_PATH_MAX + HES_PAYLOAD_MAX + 2];
    size_t n = crypto_proof_message(sign_path, sign_payload, framed, sizeof(framed));

    unsigned char sig[CRYPTO_IDENTITY_SIG_BYTES];
    unsigned char pub[CRYPTO_IDENTITY_PUB_BYTES];
    if (crypto_service_sign(signer, (const unsigned char*)framed, n, sig) != 0 ||
        crypto_service_public_key(signer, pub) != 0) {
        fprintf(stderr, "test: cannot build a proof\n");
        exit(1);
    }
    if (tamper) {
        sig[0] ^= 0x01;
    }

    char pi[65];
    char pk[CRYPTO_IDENTITY_PUB_BYTES * 2 + 1];
    char sg[CRYPTO_IDENTITY_SIG_BYTES * 2 + 1];
    to_hex(pub + 1, 32, pi);  // 'pi' is the point's X coordinate
    to_hex(pub, CRYPTO_IDENTITY_PUB_BYTES, pk);
    to_hex(sig, CRYPTO_IDENTITY_SIG_BYTES, sg);

    snprintf(out, cap, "%s;pi=%s;pk=%s;sig=%s", cred, pi, pk, sg);
}

////////////////////////////////////////////////////////////////////////////////
/// Feeds a button press carrying a credential and (usually) a proof, exactly as
/// a credential-aware sender would.
static void press(binding_map_t* bm, uint32_t di, const char* payload, const char* cred)
{
    hes_clme_msg_t in = {0};
    in.verb = HES_VERB_EVENT;
    in.device_index = di;
    hes_msg_set_path(&in, BTN_PATH);
    hes_msg_set_payload_str(&in, payload);
    hes_strlcpy(in.cred_info, sizeof(in.cred_info), cred);
    hes_strlcpy(in.user_name, sizeof(in.user_name), "parent");
    bm_processor_handle(bm, &g_bus, &in);
}

static int is_put(const hes_clme_msg_t* m, const char* path, uint32_t di, const char* payload)
{
    return m->verb == HES_VERB_PUT && m->device_index == di && strcmp(m->path, path) == 0 &&
           strcmp(m->payload, payload) == 0;
}

////////////////////////////////////////////////////////////////////////////////
/// The development switch (core's --no-authz): a row whose input declares
/// 'ap'='au' must flow with no credential at all once authorization is
/// disabled. Its own map and policy, so nothing above is disturbed.
static MunitResult test_no_authz(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    binding_map_t bm;
    bm_init(&bm);
    munit_assert_int(bm_load_xml(&bm, "data/bm_auth_two_buttons.xml"), ==, 0);

    service_object_t* auth = auth_service_create("data/auth_policy_two_buttons.json");
    munit_assert_not_null(auth);

    authorizer_t authz = {auth, NULL};
    bm.auth_ctx = &authz;
    bm.auth_authorize = bridge_authorize;

    cap_start();
    hes_clme_msg_t msgs[MAX_CAPTURED];

    // No credential, so the per-message gate refuses.
    press(&bm, BTN1_DI, "1", "");
    munit_assert_int(expect_sends(0, msgs), ==, 0);

    // Disable authorization: the same row, still without a credential, must now
    // flow. A different value, because the refused press already updated the
    // operand cache (only the output is left untraced).
    bm_disable_authorization(&bm);
    press(&bm, BTN1_DI, "0", "");
    munit_assert_int(expect_sends(1, msgs), ==, 1);
    munit_assert_true(is_put(&msgs[0], LIGHT_PATH, LIGHT_DI, "0"));

    auth->destroy(auth);
    free(auth);
    return MUNIT_OK;
}

/// The whole per-message authorization scenario, in the order this file's own
/// comments insist on: every denial uses a payload the destination has NOT
/// already got, so a suppressed send can only be the gate talking -- never the
/// binding map's own change detection. One case, because that ordering is part
/// of what is being tested.
static MunitResult test_scenario(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    binding_map_t bm;
    bm_init(&bm);
    munit_assert_int(bm_load_xml(&bm, "data/bm_auth_two_buttons.xml"), ==, 0);
    munit_assert_int(bm.n_ops, ==, 2);

    // Both rows require authorization, and both target the same light.
    munit_assert_string_equal(bm.ops[0].inputs[0].accompany_op, "au");
    munit_assert_string_equal(bm.ops[1].inputs[0].accompany_op, "au");
    munit_assert_string_equal(bm.ops[0].out_dest_object, LIGHT_PATH);
    munit_assert_string_equal(bm.ops[1].out_dest_object, LIGHT_PATH);
    munit_assert_uint(bm.ops[0].out_device_index, ==, LIGHT_DI);
    munit_assert_uint(bm.ops[1].out_device_index, ==, LIGHT_DI);

    // No static 'at' gating here: the decision is per message.
    munit_assert_char(bm.ops[0].out_author_type[0], ==, '\0');
    munit_assert_char(bm.ops[1].out_author_type[0], ==, '\0');

    // The policy names these two addresses; check the engine forms them the same.
    char addr[HES_PATH_MAX];
    bm_at_address(&bm.ops[0], addr, sizeof(addr));
    munit_assert_string_equal(addr, "/lx/ob/bm/ot1/op1/at");
    bm_at_address(&bm.ops[1], addr, sizeof(addr));
    munit_assert_string_equal(addr, "/lx/ob/bm/ot2/op1/at");

    service_object_t* auth = auth_service_create("data/auth_policy_two_buttons.json");
    munit_assert_not_null(auth);

    // Bridge the two together, as core.c does. This policy has no
    // "proofRequired" section, so the proof gate is a no-op here -- which is
    // itself worth asserting, since it means an existing deployment is
    // unaffected until someone asks for a proof.
    authorizer_t authz = {auth, NULL};
    bm.auth_ctx = &authz;
    bm.auth_authorize = bridge_authorize;

    // The GET must not leak the credential.
    hes_clme_msg_t reply = {0};
    auth->on_get(auth, &reply);
    munit_assert_not_null(strstr(reply.payload, "parent"));
    munit_assert_null(strstr(reply.payload, CRED_OK));

    cap_start();
    hes_clme_msg_t msgs[MAX_CAPTURED];

    ////////////////////////////////////////////////////////////////////////////////
    // 1. Button 1, valid credential: the permission covers this target.
    ////////////////////////////////////////////////////////////////////////////////
    // 1. Button 1, valid credential: the permission covers this target.
    press(&bm, BTN1_DI, "1", CRED_OK);
    munit_assert_int(expect_sends(1, msgs), ==, 1);
    munit_assert_true(is_put(&msgs[0], LIGHT_PATH, LIGHT_DI, "1"));

    ////////////////////////////////////////////////////////////////////////////////
    // 2. Button 2, same valid credential: no permission for that target.
    ////////////////////////////////////////////////////////////////////////////////
    // 2. Button 2, same valid credential: no permission for that target.
    press(&bm, BTN2_DI, "1", CRED_OK);
    munit_assert_int(expect_sends(0, msgs), ==, 0);

    ////////////////////////////////////////////////////////////////////////////////
    // 3. Button 1 again, wrong credential: unknown, so denied.
    ////////////////////////////////////////////////////////////////////////////////
    // 3. Button 1 again, wrong credential: unknown, so denied.
    press(&bm, BTN1_DI, "0", CRED_BAD);
    munit_assert_int(expect_sends(0, msgs), ==, 0);

    ////////////////////////////////////////////////////////////////////////////////
    // 4. And button 1 still works afterwards (no state was poisoned).
    ////////////////////////////////////////////////////////////////////////////////
    // 4. And button 1 still works afterwards (no state was poisoned).
    press(&bm, BTN1_DI, "1", CRED_OK);
    munit_assert_int(expect_sends(0, msgs), ==, 0);  // value unchanged -> change detection

    auth->destroy(auth);
    free(auth);

    ////////////////////////////////////////////////////////////////////////////////
    // Phase 2: the same scenario, with a proof gate
    ////////////////////////////////////////////////////////////////////////////////
    service_object_t* idobjs[ID_SERVICE_OBJECT_COUNT];
    munit_assert_int(id_service_create(PEER_IDENTITY, idobjs, ID_SERVICE_OBJECT_COUNT), ==,
                     ID_SERVICE_OBJECT_COUNT);

    service_object_t* crs[CRYPTO_SERVICE_OBJECT_COUNT];
    munit_assert_int(crypto_service_create(crs, CRYPTO_SERVICE_OBJECT_COUNT), ==,
                     CRYPTO_SERVICE_OBJECT_COUNT);
    service_object_t* signer = crs[CRYPTO_OBJECT_CIPHERS];

    // The peer's key. The gateway verifies with the public half that travels in
    // the proof, so it needs no key of its own for this direction.
    unsigned char peer_key[ID_IDENTITY_KEY_BYTES];
    munit_assert_int(id_service_export_identity_key(idobjs[0], peer_key), ==, 0);
    crypto_service_set_identity_key(signer, peer_key);
    memset(peer_key, 0, sizeof(peer_key));

    service_object_t* proof_auth = auth_service_create("data/auth_policy_proof.json");
    munit_assert_not_null(proof_auth);

    // The policy names an identity for button 1's row, and only that one.
    munit_assert_not_null(auth_service_required_proof_pi(proof_auth, "/lx/ob/bm/ot1/op1/at"));
    munit_assert_null(auth_service_required_proof_pi(proof_auth, "/lx/ob/bm/ot2/op1/at"));

    authz.auth = proof_auth;
    authz.crypto = signer;

    char cred[HES_CRED_MAX];

    ////////////////////////////////////////////////////////////////////////////////
    // 5. Granted, and proved: the gate opens.
    ////////////////////////////////////////////////////////////////////////////////
    // 5. Granted, and proved: the gate opens.
    build_cred(cred, sizeof(cred), CRED_OK, BTN_PATH, "0", signer, 0);
    press(&bm, BTN1_DI, "0", cred);
    munit_assert_int(expect_sends(1, msgs), ==, 1);
    munit_assert_true(is_put(&msgs[0], LIGHT_PATH, LIGHT_DI, "0"));

    ////////////////////////////////////////////////////////////////////////////////
    // 6. Granted, but no proof at all.
    ////////////////////////////////////////////////////////////////////////////////
    // 6. Granted, but no proof at all.
    press(&bm, BTN1_DI, "1", CRED_OK);
    munit_assert_int(expect_sends(0, msgs), ==, 0);

    ////////////////////////////////////////////////////////////////////////////////
    // 7. A proof whose signature does not verify.
    ////////////////////////////////////////////////////////////////////////////////
    // 7. A proof whose signature does not verify.
    build_cred(cred, sizeof(cred), CRED_OK, BTN_PATH, "2", signer, 1);  // flip a signature bit
    press(&bm, BTN1_DI, "2", cred);
    munit_assert_int(expect_sends(0, msgs), ==, 0);

    ////////////////////////////////////////////////////////////////////////////////
    // 8. A perfectly valid proof, but for the wrong identity.
    ////////////////////////////////////////////////////////////////////////////////
    // 8. A perfectly valid proof, but for the wrong identity.
    build_cred(cred, sizeof(cred), CRED_OK, BTN_PATH, "1", signer, 0);
    {
        // Replace the claimed 'pi' with one the policy does not name.
        char* at = strstr(cred, "pi=");
        munit_assert_not_null(at);
        memcpy(at + 3, OTHER_PI, 64);
    }
    press(&bm, BTN1_DI, "1", cred);
    munit_assert_int(expect_sends(0, msgs), ==, 0);

    ////////////////////////////////////////////////////////////////////////////////
    // 9. The right identity and a good signature -- over a DIFFERENT message.
    ////////////////////////////////////////////////////////////////////////////////
    // 9. The right identity and a good signature -- over a DIFFERENT message.
    build_cred(cred, sizeof(cred), CRED_OK, BTN_PATH, "0", signer, 0);  // signed 0, sent 2
    press(&bm, BTN1_DI, "2", cred);
    munit_assert_int(expect_sends(0, msgs), ==, 0);

    // A suppressed output must leave NO trace. The value cache drives every other
    // row's inputs, so if a denied press had written its result there, an
    // operation chained off this device index would act on a value the light
    // never received -- reaching the target indirectly, which is no refusal at
    // all. It must still hold what case 5 actually delivered (0), not the 1 that
    // cases 6-9 all computed.
    munit_assert_double(bm.value_cache[LIGHT_DI], ==, 0.0);

    ////////////////////////////////////////////////////////////////////////////////
    // 10. And the gate still opens, so none of the above was a stuck door.
    ////////////////////////////////////////////////////////////////////////////////
    // 10. And the gate still opens, so none of the above was a stuck door.
    build_cred(cred, sizeof(cred), CRED_OK, BTN_PATH, "1", signer, 0);
    press(&bm, BTN1_DI, "1", cred);
    munit_assert_int(expect_sends(1, msgs), ==, 1);
    munit_assert_true(is_put(&msgs[0], LIGHT_PATH, LIGHT_DI, "1"));

    proof_auth->destroy(proof_auth);
    free(proof_auth);
    for (int i = 0; i < CRYPTO_SERVICE_OBJECT_COUNT; i++) {
        crs[i]->destroy(crs[i]);
        free(crs[i]);
    }
    return MUNIT_OK;
}

static MunitTest auth_authorize_tests[] = {
  { (char*)"/scenario", test_scenario, NULL, capture_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/no-authz", test_no_authz, NULL, capture_tear_down, MUNIT_TEST_OPTION_NONE, NULL },
  { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite auth_authorize_suite = {
  (char*)"/auth-authorize", auth_authorize_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char* argv[])
{
  return munit_suite_main(&auth_authorize_suite, NULL, argc, argv);
}
