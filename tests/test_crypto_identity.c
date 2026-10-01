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
/// @brief Offline test for the identity key pair: the fingerprint becomes a
/// signing key, and the public ID becomes verifiable.
///
/// @details
/// ISO/IEC 18012-3 11.2.2.1 says the public ID exists "so that masquerade
/// gateways cannot pretend to be legitimate gateways", and that the
/// digitalFingerprint is "a secret internal random code ... used for encryption
/// techniques". The shape that satisfies both is a key pair derived from 'fp',
/// with 'pi' as the public half. This test checks that end to end:
///
///   1. the identification service derives a key and serves 'pi' as the public
///      X coordinate;
///   2. handover is one-shot -- after the crypto service takes the key, the id
///      service has nothing left to give;
///   3. a message signed with it verifies against its public key;
///   4. a tampered message, a tampered signature, or a tampered key does not;
///   5. signing is DETERMINISTIC, so the same message always gives the same
///      signature (this is what makes the test possible at all, and it removes
///      the nonce-reuse hazard ordinary ECDSA has);
///   6. 'pi' pins the key: a key whose X differs from the claimed 'pi' is not
///      the peer it claims to be.
///
/// No randomness, no network, no hardware.
///
/// Build/run: see tests/CMakeLists.txt (ctest).

#include "services/crypto/crypto.h"
#include "services/id/id.h"

#include "common/hes_common.h"
#include "common/service_object.h"

#include <munit.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// The demo identity from tests/data/identity.json (run from the tests source
/// directory).
#define DEMO_IDENTITY "data/identity.json"

static const unsigned char CHALLENGE[] = "openhes challenge: prove you are gateway X";

////////////////////////////////////////////////////////////////////////////////
/// Reads the value of one datum out of a 'transCode=value;...' answer.
///
/// WARNING: the value comes back in ONE static buffer, so it is only valid until
/// the next call -- copy it out before asking for another. The buffer is sized
/// HES_PAYLOAD_MAX because a value can be a whole 130-character public key.
static const char* datum_value(const char* payload, const char* code)
{
    static char value[HES_PAYLOAD_MAX];
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
/// Decodes hex (either case) into bytes. Returns the byte count, or -1.
static int from_hex(const char* hex, unsigned char* out, size_t out_cap)
{
    size_t len = strlen(hex);
    if (len == 0 || (len % 2) != 0 || len / 2 > out_cap) {
        return -1;
    }
    for (size_t i = 0; i < len / 2; i++) {
        unsigned int b = 0;
        if (sscanf(hex + 2 * i, "%2x", &b) != 1) {
            return -1;
        }
        out[i] = (unsigned char)b;
    }
    return (int)(len / 2);
}

/// What the tests below need: the identification service, the key it hands over
/// exactly once, and the crypto service that receives it.
typedef struct identity_fixture {
    service_object_t* idobjs[ID_SERVICE_OBJECT_COUNT];
    service_object_t* crypto_objs[CRYPTO_SERVICE_OBJECT_COUNT];
    char pi[65];  ///< the public ID, COPIED out of a GET answer
} identity_fixture_t;

////////////////////////////////////////////////////////////////////////////////
/// Starts a fixture. The key transfer is one-shot, so this can only be done once
/// per service instance -- which is why every test builds its own.
static void fixture_start(identity_fixture_t* f)
{
    if (id_service_create(DEMO_IDENTITY, f->idobjs, ID_SERVICE_OBJECT_COUNT) !=
        ID_SERVICE_OBJECT_COUNT) {
        munit_errorf("cannot load %s", DEMO_IDENTITY);
    }
    if (crypto_service_create(f->crypto_objs, CRYPTO_SERVICE_OBJECT_COUNT) !=
        CRYPTO_SERVICE_OBJECT_COUNT) {
        munit_error("cannot create the crypto service");
    }

    unsigned char key[ID_IDENTITY_KEY_BYTES];
    if (id_service_export_identity_key(f->idobjs[0], key) != 0) {
        munit_error("the identification service handed over no key");
    }
    crypto_service_set_identity_key(f->crypto_objs[CRYPTO_OBJECT_CIPHERS], key);
    memset(key, 0, sizeof(key));

    // 'pi' is COPIED out, not held as a bare pointer: datum_value() reuses one
    // static buffer, so a pointer read here would silently become whatever
    // value was fetched last. That is not hypothetical -- it is how an earlier
    // version of this test ended up comparing a proof against the signature
    // instead of against the identity.
    hes_clme_msg_t status = {0};
    f->idobjs[1]->on_get(f->idobjs[1], &status);
    const char* served = datum_value(status.payload, "pi");
    if (served == NULL || strlen(served) != 64) {
        munit_errorf("no 64-character 'pi' in the status answer: %s", status.payload);
    }
    hes_strlcpy(f->pi, sizeof(f->pi), served);
}

static void fixture_stop(identity_fixture_t* f)
{
    for (int i = 0; i < ID_SERVICE_OBJECT_COUNT; i++) {
        f->idobjs[i]->destroy(f->idobjs[i]);
        free(f->idobjs[i]);
    }
    for (int i = 0; i < CRYPTO_SERVICE_OBJECT_COUNT; i++) {
        f->crypto_objs[i]->destroy(f->crypto_objs[i]);
        free(f->crypto_objs[i]);
    }
}

/// The identity key is handed over once and then gone: the id service wipes its
/// copy, so a second export cannot succeed.
static MunitResult test_key_transfer(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    service_object_t* idobjs[ID_SERVICE_OBJECT_COUNT];
    munit_assert_int(id_service_create(DEMO_IDENTITY, idobjs, ID_SERVICE_OBJECT_COUNT), ==,
                     ID_SERVICE_OBJECT_COUNT);

    unsigned char key[ID_IDENTITY_KEY_BYTES];
    munit_assert_int(id_service_export_identity_key(idobjs[0], key), ==, 0);
    munit_assert_int(id_service_export_identity_key(idobjs[0], key), ==, -1);
    memset(key, 0, sizeof(key));

    for (int i = 0; i < ID_SERVICE_OBJECT_COUNT; i++) {
        idobjs[i]->destroy(idobjs[i]);
        free(idobjs[i]);
    }
    return MUNIT_OK;
}

/// The public key the gateway presents is the point 'pi' names: uncompressed,
/// with an even Y -- the canonicalisation rule that lets 'pi' alone name it.
static MunitResult test_public_identity(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    identity_fixture_t f;
    fixture_start(&f);

    service_object_t* crypto = f.crypto_objs[CRYPTO_OBJECT_CIPHERS];
    unsigned char pub[CRYPTO_IDENTITY_PUB_BYTES];
    munit_assert_int(crypto_service_public_key(crypto, pub), ==, 0);
    munit_assert_int(pub[0], ==, 0x04);  // uncompressed point
    munit_assert_int(pub[CRYPTO_IDENTITY_PUB_BYTES - 1] & 1, ==, 0);
    munit_assert_int(crypto_pi_matches(pub, f.pi), ==, 1);

    fixture_stop(&f);
    return MUNIT_OK;
}

/// The capability address (Tables 70/71) answers 'si=no' until a key is set --
/// the same service seen through its other door.
static MunitResult test_capability_before_key(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    service_object_t* objs[CRYPTO_SERVICE_OBJECT_COUNT];
    munit_assert_int(crypto_service_create(objs, CRYPTO_SERVICE_OBJECT_COUNT), ==,
                     CRYPTO_SERVICE_OBJECT_COUNT);

    hes_clme_msg_t capability = {0};
    objs[CRYPTO_OBJECT_CONFIG]->on_get(objs[CRYPTO_OBJECT_CONFIG], &capability);
    munit_assert_string_equal(capability.payload, "vr=1.0.0;rq=ma;si=no");

    for (int i = 0; i < CRYPTO_SERVICE_OBJECT_COUNT; i++) {
        objs[i]->destroy(objs[i]);
        free(objs[i]);
    }
    return MUNIT_OK;
}

/// Sign, then verify with the presented public key -- and refuse a tampered
/// message, signature or key.
static MunitResult test_sign_verify(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    identity_fixture_t f;
    fixture_start(&f);

    service_object_t* crypto = f.crypto_objs[CRYPTO_OBJECT_CIPHERS];
    unsigned char pub[CRYPTO_IDENTITY_PUB_BYTES];
    munit_assert_int(crypto_service_public_key(crypto, pub), ==, 0);

    unsigned char sig[CRYPTO_IDENTITY_SIG_BYTES];
    munit_assert_int(crypto_service_sign(crypto, CHALLENGE, sizeof(CHALLENGE) - 1, sig), ==, 0);
    munit_assert_int(crypto_verify_message(pub, CHALLENGE, sizeof(CHALLENGE) - 1, sig), ==, 0);

    unsigned char tampered[sizeof(CHALLENGE)];
    memcpy(tampered, CHALLENGE, sizeof(tampered));
    tampered[0] ^= 0x01;
    munit_assert_int(crypto_verify_message(pub, tampered, sizeof(CHALLENGE) - 1, sig), !=, 0);

    unsigned char bad_sig[CRYPTO_IDENTITY_SIG_BYTES];
    memcpy(bad_sig, sig, sizeof(bad_sig));
    bad_sig[0] ^= 0x01;
    munit_assert_int(crypto_verify_message(pub, CHALLENGE, sizeof(CHALLENGE) - 1, bad_sig), !=, 0);

    unsigned char bad_pub[CRYPTO_IDENTITY_PUB_BYTES];
    memcpy(bad_pub, pub, sizeof(bad_pub));
    bad_pub[1] ^= 0x01;  // X no longer the peer's
    munit_assert_int(crypto_verify_message(bad_pub, CHALLENGE, sizeof(CHALLENGE) - 1, sig), !=, 0);
    // ... and pinning notices before any crypto is done.
    munit_assert_int(crypto_pi_matches(bad_pub, f.pi), ==, 0);

    // A key that is not an uncompressed point is refused outright.
    unsigned char not_a_point[CRYPTO_IDENTITY_PUB_BYTES];
    memcpy(not_a_point, pub, sizeof(not_a_point));
    not_a_point[0] = 0x02;
    munit_assert_int(
            crypto_verify_message(not_a_point, CHALLENGE, sizeof(CHALLENGE) - 1, sig), !=, 0);
    munit_assert_int(crypto_pi_matches(not_a_point, f.pi), ==, 0);

    fixture_stop(&f);
    return MUNIT_OK;
}

/// Both halves are deterministic: signing twice gives the same bytes, and the
/// derivation gives the same 'pi' every time, so the public ID is stationary.
static MunitResult test_deterministic(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    identity_fixture_t f;
    fixture_start(&f);

    service_object_t* crypto = f.crypto_objs[CRYPTO_OBJECT_CIPHERS];
    unsigned char sig[CRYPTO_IDENTITY_SIG_BYTES];
    unsigned char sig2[CRYPTO_IDENTITY_SIG_BYTES];
    munit_assert_int(crypto_service_sign(crypto, CHALLENGE, sizeof(CHALLENGE) - 1, sig), ==, 0);
    munit_assert_int(crypto_service_sign(crypto, CHALLENGE, sizeof(CHALLENGE) - 1, sig2), ==, 0);
    munit_assert_memory_equal(sizeof(sig), sig, sig2);

    // A different message gives a different signature.
    static const unsigned char OTHER[] = "another challenge";
    munit_assert_int(crypto_service_sign(crypto, OTHER, sizeof(OTHER) - 1, sig2), ==, 0);
    munit_assert_memory_not_equal(sizeof(sig), sig, sig2);

    // The derivation is deterministic: the same document yields the same 'pi'.
    service_object_t* again[ID_SERVICE_OBJECT_COUNT];
    munit_assert_int(id_service_create(DEMO_IDENTITY, again, ID_SERVICE_OBJECT_COUNT), ==,
                     ID_SERVICE_OBJECT_COUNT);
    hes_clme_msg_t status2 = {0};
    again[1]->on_get(again[1], &status2);
    munit_assert_string_equal(f.pi, datum_value(status2.payload, "pi"));
    for (int i = 0; i < ID_SERVICE_OBJECT_COUNT; i++) {
        again[i]->destroy(again[i]);
        free(again[i]);
    }

    fixture_stop(&f);
    return MUNIT_OK;
}

/// The proof a verifier asks for: a challenge in, a signature out -- and never
/// a signature without a challenge.
static MunitResult test_proof(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    identity_fixture_t f;
    fixture_start(&f);

    service_object_t* crypto = f.crypto_objs[CRYPTO_OBJECT_CIPHERS];
    static const char CHALLENGE_HEX[] = "deadbeefcafe0123456789abcdef0011";

    hes_clme_msg_t req = {0};
    hes_strlcpy(req.query, sizeof(req.query), "ei=100");
    hes_msg_set_payload_str(&req, CHALLENGE_HEX);
    crypto->on_get(crypto, &req);

    // datum_value() hands back one static buffer, so copy each value out
    // before asking for the next one.
    char proof_pi[65];
    char proof_pk[CRYPTO_IDENTITY_PUB_BYTES * 2 + 1];
    char proof_sig[CRYPTO_IDENTITY_SIG_BYTES * 2 + 1];
    const char* v = datum_value(req.payload, "pi");
    munit_assert_not_null(v);
    hes_strlcpy(proof_pi, sizeof(proof_pi), v);
    v = datum_value(req.payload, "pk");
    munit_assert_not_null(v);
    hes_strlcpy(proof_pk, sizeof(proof_pk), v);
    v = datum_value(req.payload, "sig");
    munit_assert_not_null(v);
    hes_strlcpy(proof_sig, sizeof(proof_sig), v);

    // The crypto service derives the same public identity the id service
    // serves -- they must agree, or no verifier could pin the key.
    munit_assert_string_equal(proof_pi, f.pi);

    unsigned char proof_pub[CRYPTO_IDENTITY_PUB_BYTES];
    unsigned char proof_sig_bytes[CRYPTO_IDENTITY_SIG_BYTES];
    unsigned char challenge[32];
    int n_pub = from_hex(proof_pk, proof_pub, sizeof(proof_pub));
    int n_sig = from_hex(proof_sig, proof_sig_bytes, sizeof(proof_sig_bytes));
    int n_ch = from_hex(CHALLENGE_HEX, challenge, sizeof(challenge));
    munit_assert_int(n_pub, ==, CRYPTO_IDENTITY_PUB_BYTES);
    munit_assert_int(n_sig, ==, CRYPTO_IDENTITY_SIG_BYTES);
    munit_assert_int(n_ch, >, 0);

    // The whole point: a verifier that holds only the public ID can check
    // the proof -- the key matches the claimed identity, and the signature
    // is over the challenge it sent.
    munit_assert_int(crypto_pi_matches(proof_pub, proof_pi), ==, 1);
    munit_assert_int(crypto_verify_message(proof_pub, challenge, (size_t)n_ch, proof_sig_bytes), ==,
                     0);

    // The same proof does not answer a different challenge, so a recorded
    // one cannot be replayed against a fresh nonce.
    challenge[0] ^= 0x01;
    munit_assert_int(
            crypto_verify_message(proof_pub, challenge, (size_t)n_ch, proof_sig_bytes), !=, 0);
    challenge[0] ^= 0x01;

    // A request with no challenge gets the capability report, never a
    // signature: guessing at a challenge would defeat the purpose.
    hes_clme_msg_t no_challenge = {0};
    hes_strlcpy(no_challenge.query, sizeof(no_challenge.query), "ei=100");
    crypto->on_get(crypto, &no_challenge);
    munit_assert_null(datum_value(no_challenge.payload, "sig"));
    munit_assert_not_null(datum_value(no_challenge.payload, "it"));

    // A malformed challenge is refused outright.
    hes_clme_msg_t bad = {0};
    hes_strlcpy(bad.query, sizeof(bad.query), "ei=100");
    hes_msg_set_payload_str(&bad, "not-hex");
    crypto->on_get(crypto, &bad);
    munit_assert_string_equal(bad.payload, "error:bad-challenge");

    fixture_stop(&f);
    return MUNIT_OK;
}

/// The Table 73 row that describes the operation, as the service reports it.
static MunitResult test_table_73(const MunitParameter params[], void* user_data)
{
    (void)params;
    (void)user_data;

    identity_fixture_t f;
    fixture_start(&f);

    service_object_t* crypto = f.crypto_objs[CRYPTO_OBJECT_CIPHERS];
    hes_clme_msg_t caps = {0};
    hes_strlcpy(caps.query, sizeof(caps.query), "ei=100");
    crypto->on_get(crypto, &caps);
    munit_assert_not_null(datum_value(caps.payload, "ks"));
    munit_assert_string_equal(datum_value(caps.payload, "ks"), "32");
    munit_assert_string_equal(datum_value(caps.payload, "it"), "256");
    munit_assert_string_equal(datum_value(caps.payload, "au"), "64");

    fixture_stop(&f);
    return MUNIT_OK;
}

static MunitTest crypto_identity_tests[] = {
  { (char*)"/key-transfer", test_key_transfer, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/public-identity", test_public_identity, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/capability-before-key", test_capability_before_key, NULL, NULL,
    MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/sign-verify", test_sign_verify, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/deterministic", test_deterministic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/proof", test_proof, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { (char*)"/table-73", test_table_73, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
  { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite crypto_identity_suite = {
  (char*)"/crypto-identity", crypto_identity_tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int main(int argc, char* argv[])
{
  return munit_suite_main(&crypto_identity_suite, NULL, argc, argv);
}


