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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

/// The demo identity from tests/identity.json (run from this directory).
#define DEMO_IDENTITY "identity.json"

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

int main(void)
{
    service_object_t* idobjs[ID_SERVICE_OBJECT_COUNT];
    CHECK(id_service_create(DEMO_IDENTITY, idobjs, ID_SERVICE_OBJECT_COUNT) ==
          ID_SERVICE_OBJECT_COUNT);

    ////////////////////////////////////////////////////////////////////////////////
    // 1. 'pi' is the derived public key, served on the status address
    ////////////////////////////////////////////////////////////////////////////////
    hes_clme_msg_t status = {0};
    idobjs[1]->on_get(idobjs[1], &status);

    // 'pi' is COPIED out, not held as a bare pointer: datum_value() reuses one
    // static buffer, so a pointer read here would silently become whatever
    // value was fetched last. That is not hypothetical -- it is how an earlier
    // version of this test ended up comparing a proof against the signature
    // instead of against the identity.
    char pi_storage[65];
    {
        const char* served = datum_value(status.payload, "pi");
        CHECK(served != NULL);
        CHECK(strlen(served) == 64);
        hes_strlcpy(pi_storage, sizeof(pi_storage), served);
    }
    const char* pi = pi_storage;

    ////////////////////////////////////////////////////////////////////////////////
    // 2. hand the key over; it is a one-shot transfer
    ////////////////////////////////////////////////////////////////////////////////
    unsigned char key[ID_IDENTITY_KEY_BYTES];
    CHECK(id_service_export_identity_key(idobjs[0], key) == 0);
    // Nothing left to give: the id service wiped its copy.
    CHECK(id_service_export_identity_key(idobjs[0], key) == -1);

    service_object_t* crypto_objs[CRYPTO_SERVICE_OBJECT_COUNT];
    CHECK(crypto_service_create(crypto_objs, CRYPTO_SERVICE_OBJECT_COUNT) ==
          CRYPTO_SERVICE_OBJECT_COUNT);
    service_object_t* crypto = crypto_objs[CRYPTO_OBJECT_CIPHERS];
    CHECK(crypto != NULL);

    ////////////////////////////////////////////////////////////////////////////////
    // the capability address (Tables 70/71), same service, other door
    ////////////////////////////////////////////////////////////////////////////////
    hes_clme_msg_t capability = {0};
    crypto_objs[CRYPTO_OBJECT_CONFIG]->on_get(crypto_objs[CRYPTO_OBJECT_CONFIG], &capability);
    CHECK(strcmp(capability.payload, "vr=1.0.0;rq=ma;si=no") == 0);
    crypto_service_set_identity_key(crypto, key);
    memset(key, 0, sizeof(key));

    ////////////////////////////////////////////////////////////////////////////////
    // the public key the gateway presents, whose X must equal 'pi'
    ////////////////////////////////////////////////////////////////////////////////
    unsigned char pub[CRYPTO_IDENTITY_PUB_BYTES];
    CHECK(crypto_service_public_key(crypto, pub) == 0);
    CHECK(pub[0] ==
          0x04);  // uncompressed point
                  // The canonicalisation rule: Y is the even root, so 'pi' alone names it.
    CHECK((pub[CRYPTO_IDENTITY_PUB_BYTES - 1] & 1) == 0);
    CHECK(crypto_pi_matches(pub, pi) == 1);

    ////////////////////////////////////////////////////////////////////////////////
    // 3. sign, then verify with the presented public key
    ////////////////////////////////////////////////////////////////////////////////
    unsigned char sig[CRYPTO_IDENTITY_SIG_BYTES];
    CHECK(crypto_service_sign(crypto, CHALLENGE, sizeof(CHALLENGE) - 1, sig) == 0);
    CHECK(crypto_verify_message(pub, CHALLENGE, sizeof(CHALLENGE) - 1, sig) == 0);

    ////////////////////////////////////////////////////////////////////////////////
    // 4. a tampered message, signature, or key must fail
    ////////////////////////////////////////////////////////////////////////////////
    unsigned char tampered[sizeof(CHALLENGE)];
    memcpy(tampered, CHALLENGE, sizeof(tampered));
    tampered[0] ^= 0x01;
    CHECK(crypto_verify_message(pub, tampered, sizeof(CHALLENGE) - 1, sig) != 0);

    unsigned char bad_sig[CRYPTO_IDENTITY_SIG_BYTES];
    memcpy(bad_sig, sig, sizeof(bad_sig));
    bad_sig[0] ^= 0x01;
    CHECK(crypto_verify_message(pub, CHALLENGE, sizeof(CHALLENGE) - 1, bad_sig) != 0);

    unsigned char bad_pub[CRYPTO_IDENTITY_PUB_BYTES];
    memcpy(bad_pub, pub, sizeof(bad_pub));
    bad_pub[1] ^= 0x01;  // X no longer the peer's
    CHECK(crypto_verify_message(bad_pub, CHALLENGE, sizeof(CHALLENGE) - 1, sig) != 0);
    // ... and pinning notices before any crypto is done.
    CHECK(crypto_pi_matches(bad_pub, pi) == 0);

    // A key that is not an uncompressed point is refused outright.
    unsigned char not_a_point[CRYPTO_IDENTITY_PUB_BYTES];
    memcpy(not_a_point, pub, sizeof(not_a_point));
    not_a_point[0] = 0x02;
    CHECK(crypto_verify_message(not_a_point, CHALLENGE, sizeof(CHALLENGE) - 1, sig) != 0);
    CHECK(crypto_pi_matches(not_a_point, pi) == 0);

    ////////////////////////////////////////////////////////////////////////////////
    // 5. signing is deterministic
    ////////////////////////////////////////////////////////////////////////////////
    unsigned char sig2[CRYPTO_IDENTITY_SIG_BYTES];
    CHECK(crypto_service_sign(crypto, CHALLENGE, sizeof(CHALLENGE) - 1, sig2) == 0);
    CHECK(memcmp(sig, sig2, sizeof(sig)) == 0);

    // A different message gives a different signature.
    static const unsigned char OTHER[] = "another challenge";
    CHECK(crypto_service_sign(crypto, OTHER, sizeof(OTHER) - 1, sig2) == 0);
    CHECK(memcmp(sig, sig2, sizeof(sig)) != 0);

    ////////////////////////////////////////////////////////////////////////////////
    // 6. the derivation is deterministic, so 'pi' is stationary
    ////////////////////////////////////////////////////////////////////////////////
    service_object_t* again[ID_SERVICE_OBJECT_COUNT];
    CHECK(id_service_create(DEMO_IDENTITY, again, ID_SERVICE_OBJECT_COUNT) ==
          ID_SERVICE_OBJECT_COUNT);
    hes_clme_msg_t status2 = {0};
    again[1]->on_get(again[1], &status2);
    CHECK(strcmp(pi, datum_value(status2.payload, "pi")) == 0);

    ////////////////////////////////////////////////////////////////////////////////
    // the proof a verifier asks for: a challenge in, a signature out
    ////////////////////////////////////////////////////////////////////////////////
    {
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
        CHECK(v != NULL);
        hes_strlcpy(proof_pi, sizeof(proof_pi), v);
        v = datum_value(req.payload, "pk");
        CHECK(v != NULL);
        hes_strlcpy(proof_pk, sizeof(proof_pk), v);
        v = datum_value(req.payload, "sig");
        CHECK(v != NULL);
        hes_strlcpy(proof_sig, sizeof(proof_sig), v);

        // The crypto service derives the same public identity the id service
        // serves -- they must agree, or no verifier could pin the key.
        CHECK(strcmp(proof_pi, pi) == 0);

        unsigned char proof_pub[CRYPTO_IDENTITY_PUB_BYTES];
        unsigned char proof_sig_bytes[CRYPTO_IDENTITY_SIG_BYTES];
        unsigned char challenge[32];
        int n_pub = from_hex(proof_pk, proof_pub, sizeof(proof_pub));
        int n_sig = from_hex(proof_sig, proof_sig_bytes, sizeof(proof_sig_bytes));
        int n_ch = from_hex(CHALLENGE_HEX, challenge, sizeof(challenge));
        CHECK(n_pub == CRYPTO_IDENTITY_PUB_BYTES);
        CHECK(n_sig == CRYPTO_IDENTITY_SIG_BYTES);
        CHECK(n_ch > 0);

        // The whole point: a verifier that holds only the public ID can check
        // the proof -- the key matches the claimed identity, and the signature
        // is over the challenge it sent.
        CHECK(crypto_pi_matches(proof_pub, proof_pi) == 1);
        CHECK(crypto_verify_message(proof_pub, challenge, (size_t)n_ch, proof_sig_bytes) == 0);

        // The same proof does not answer a different challenge, so a recorded
        // one cannot be replayed against a fresh nonce.
        challenge[0] ^= 0x01;
        CHECK(crypto_verify_message(proof_pub, challenge, (size_t)n_ch, proof_sig_bytes) != 0);

        // A request with no challenge gets the capability report, never a
        // signature: guessing at a challenge would defeat the purpose.
        hes_clme_msg_t no_challenge = {0};
        hes_strlcpy(no_challenge.query, sizeof(no_challenge.query), "ei=100");
        crypto->on_get(crypto, &no_challenge);
        CHECK(datum_value(no_challenge.payload, "sig") == NULL);
        CHECK(datum_value(no_challenge.payload, "it") != NULL);

        // A malformed challenge is refused outright.
        hes_clme_msg_t bad = {0};
        hes_strlcpy(bad.query, sizeof(bad.query), "ei=100");
        hes_msg_set_payload_str(&bad, "not-hex");
        crypto->on_get(crypto, &bad);
        CHECK(strcmp(bad.payload, "error:bad-challenge") == 0);
    }

    ////////////////////////////////////////////////////////////////////////////////
    // the Table 73 row that describes the operation
    ////////////////////////////////////////////////////////////////////////////////
    hes_clme_msg_t caps = {0};
    hes_strlcpy(caps.query, sizeof(caps.query), "ei=100");
    crypto->on_get(crypto, &caps);
    CHECK(datum_value(caps.payload, "ks") != NULL);
    CHECK(strcmp(datum_value(caps.payload, "ks"), "32") == 0);
    CHECK(strcmp(datum_value(caps.payload, "it"), "256") == 0);
    CHECK(strcmp(datum_value(caps.payload, "au"), "64") == 0);

    for (int i = 0; i < ID_SERVICE_OBJECT_COUNT; i++) {
        idobjs[i]->destroy(idobjs[i]);
        free(idobjs[i]);
        again[i]->destroy(again[i]);
        free(again[i]);
    }
    crypto->destroy(crypto);
    free(crypto);

    printf("test_crypto_identity: ok\n");
    return 0;
}
