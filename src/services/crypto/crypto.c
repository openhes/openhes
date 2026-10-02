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
/// @brief Implementation of the cryptographic service (crypto.h): the cipher
/// table, the AES/ARIA plumbing, and the identity signature operation.
///
/// @details
/// Two things live here:
///
///   - the cipher rows: one cipher_entry per encryptIndex ('ei'), holding the
///     algorithm ('nc'), the mode ('mc') and the key size ('ks'), plus the key
///     material itself. Blowfish is advertised as unsupported because this Mbed
///     TLS build has no such primitive, and a row whose 'it' is non-zero is an
///     internal operation rather than a wire cipher.
///
///   - the crypto work, through Mbed TLS PSA Crypto: the block modes (CBC/CFB),
///     the AEAD modes (CCM/GCM -- 12-byte nonce, 16-byte tag, no additional
///     authenticated data), and the identity-key operations the identification
///     service depends on.
///
/// An unsupported algorithm, mode or combination, an invalid parameter, or a
/// failed AEAD authentication is reported as a service-level error -- never as a
/// partial answer.

#include "crypto.h"

#include <log.h>
#include <psa/crypto.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CRYPTO_MAX_CIPHERS 16
#define CRYPTO_MAX_KEY 32
#define CRYPTO_MAX_PAYLOAD 128
#define CRYPTO_AEAD_NONCE_SIZE 12
#define CRYPTO_AEAD_TAG_SIZE 16

typedef struct cipher_entry {
    int ei;               ///< encryptIndex, 'ei' -- matches the ?ei=N query param
    char name_cipher[4];  ///< 'nc': ae/ar/bf/... (18012-3 table 77)
    int key_size;         ///< 'ks', bytes
    char mode_cipher[4];  ///< 'mc': cb/cc/cf/gc
    unsigned char key[CRYPTO_MAX_KEY];
    int supported;

    /// Table 73's two fields for an operation that is not a wire cipher:
    /// 'it' internal operations (bits) and 'au' authorization (bytes). A row
    /// with 'it' > 0 is such an operation, and reports these instead of
    /// 'nc'/'mc'.
    int internal_bits;  ///< 'it'
    int auth_bytes;     ///< 'au'
} cipher_entry_t;

typedef struct crypto_service_state {
    cipher_entry_t ciphers[CRYPTO_MAX_CIPHERS];
    int n_ciphers;

    /// The identity key derived from the identification service's 'fp'
    /// (11.2.2.1: the fingerprint is "used for encryption techniques"). It is
    /// held HERE because this is the component that performs cryptographic
    /// operations -- the id service wipes its own copy on handover, so exactly
    /// one copy of the secret exists.
    unsigned char identity_scalar[CRYPTO_IDENTITY_KEY_BYTES];
    int have_identity_key;

    /// The objects at both addresses share this state, so only the last one out
    /// scrubs the secret and frees it (see services/id/id.c).
    int refs;
} crypto_service_state_t;

////////////////////////////////////////////////////////////////////////////////
/// Finds the cipher table row for an 'ei' (encryptionIndex).
///
/// @param st The service state holding the table.
/// @param ei The encryptionIndex from the request's query.
/// @return The row, or NULL when no row carries that index.
static const cipher_entry_t* find_cipher(const crypto_service_state_t* st, int ei)
{
    for (int i = 0; i < st->n_ciphers; i++) {
        if (st->ciphers[i].ei == ei) {
            return &st->ciphers[i];
        }
    }
    return NULL;
}

////////////////////////////////////////////////////////////////////////////////
/// Reads the encryptionIndex out of a request query, e.g. "ei=1".
///
/// @param query The request query text.
/// @return The index, or -1 when the query carries no "ei=".
static int parse_ei(const char* query)
{
    const char* p = strstr(query, "ei=");
    if (!p) {
        return -1;
    }
    return atoi(p + 3);
}

////////////////////////////////////////////////////////////////////////////////
/// Decodes a hex string into bytes. The text must have an even number of digits
/// and fit in the caller's buffer.
///
/// @param hex     The hex text.
/// @param out     Receives the decoded bytes.
/// @param out_cap Capacity of out, in bytes.
/// @return How many bytes were decoded, or -1 on odd length, overflow, or a
///         character that is not a hex digit.
static int hex_decode(const char* hex, unsigned char* out, size_t out_cap)
{
    size_t hex_len = strlen(hex);
    if ((hex_len % 2) != 0) {
        return -1;
    }
    size_t n = hex_len / 2;
    if (n > out_cap) {
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned int b;
        char pair[3] = {hex[2 * i], hex[2 * i + 1], '\0'};
        if (sscanf(pair, "%2x", &b) != 1) {
            return -1;
        }
        out[i] = (unsigned char)b;
    }
    return (int)n;
}

////////////////////////////////////////////////////////////////////////////////
/// Encodes bytes as upper-case hex, NUL-terminated, stopping when the caller's
/// buffer can hold no more (room for the terminator is always kept).
///
/// @param data    The bytes to encode.
/// @param len     How many bytes to encode.
/// @param out     Receives the hex text.
/// @param out_cap Capacity of out.
static void hex_encode(const unsigned char* data, int len, char* out, size_t out_cap)
{
    size_t off = 0;
    for (int i = 0; i < len && off + 2 < out_cap; i++) {
        off += (size_t)snprintf(out + off, out_cap - off, "%02X", data[i]);
    }
    out[off] = '\0';
}

////////////////////////////////////////////////////////////////////////////////
/// Maps a row's 'nc' (nameCipher) to the PSA key type it needs.
///
/// @param c The cipher row.
/// @return PSA_KEY_TYPE_AES or PSA_KEY_TYPE_ARIA, or PSA_KEY_TYPE_NONE when the
///         cipher is not one we support.
static psa_key_type_t psa_cipher_key_type(const cipher_entry_t* c)
{
    if (strcmp(c->name_cipher, "ae") == 0) {
        return PSA_KEY_TYPE_AES;
    }
    if (strcmp(c->name_cipher, "ar") == 0) {
        return PSA_KEY_TYPE_ARIA;
    }
    return PSA_KEY_TYPE_NONE;
}

////////////////////////////////////////////////////////////////////////////////
/// Maps a row's 'mc' (modeCipher) to the PSA algorithm it needs.
///
/// @param c The cipher row.
/// @return The PSA algorithm, or PSA_ALG_NONE when the mode is not one we
///         support.
static psa_algorithm_t psa_cipher_algorithm(const cipher_entry_t* c)
{
    if (strcmp(c->mode_cipher, "cb") == 0) {
        return PSA_ALG_CBC_NO_PADDING;
    }
    if (strcmp(c->mode_cipher, "cf") == 0) {
        return PSA_ALG_CFB;
    }
    if (strcmp(c->mode_cipher, "cc") == 0) {
        return PSA_ALG_CCM;
    }
    if (strcmp(c->mode_cipher, "gc") == 0) {
        return PSA_ALG_GCM;
    }
    return PSA_ALG_NONE;
}

////////////////////////////////////////////////////////////////////////////////
/// Imports a row's key into PSA and hands back its handle. The key may only
/// decrypt, and only with that row's own algorithm, so a key can never be used
/// for something the row does not describe.
///
/// @param c      The cipher row whose key is imported.
/// @param key_id Receives the PSA key handle.
/// @return PSA_SUCCESS, or the error the import reported.
static psa_status_t import_key(const cipher_entry_t* c, psa_key_id_t* key_id)
{
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes, psa_cipher_key_type(c));
    psa_set_key_bits(&attributes, (psa_key_bits_t)(c->key_size * 8));
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, psa_cipher_algorithm(c));
    psa_status_t status = psa_import_key(&attributes, c->key, (size_t)c->key_size, key_id);
    psa_reset_key_attributes(&attributes);
    return status;
}

////////////////////////////////////////////////////////////////////////////////
/// Decrypts in place with a block or feedback mode (CBC without padding, CFB).
/// Such a mode has no authentication tag, so a wrong key shows up as garbage
/// plaintext rather than as an error.
///
/// The IV is all zeroes, as this POC's documents assume. The output must come out
/// exactly as long as the input, or the call fails.
///
/// @param data The ciphertext; on success it holds the plaintext.
/// @param len  Its length; a multiple of 16 for CBC.
/// @param c    The cipher row (algorithm and key).
/// @return PSA_SUCCESS, or the error that stopped the operation.
static psa_status_t decrypt_block_or_feedback(unsigned char* data,
                                              size_t len,
                                              const cipher_entry_t* c)
{
    if (len == 0 || (strcmp(c->mode_cipher, "cb") == 0 && len % 16 != 0)) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        return status;
    }

    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    status = import_key(c, &key_id);
    if (status != PSA_SUCCESS) {
        return status;
    }

    psa_cipher_operation_t operation = PSA_CIPHER_OPERATION_INIT;
    status = psa_cipher_decrypt_setup(&operation, key_id, psa_cipher_algorithm(c));
    unsigned char iv[16] = {0};
    if (status == PSA_SUCCESS) {
        status = psa_cipher_set_iv(&operation, iv, sizeof(iv));
    }

    unsigned char output[128];
    size_t output_len = 0;
    size_t finish_len = 0;
    if (status == PSA_SUCCESS) {
        status = psa_cipher_update(&operation, data, len, output, sizeof(output), &output_len);
    }
    if (status == PSA_SUCCESS) {
        status = psa_cipher_finish(&operation, output + output_len, sizeof(output) - output_len,
                                   &finish_len);
    }
    if (status == PSA_SUCCESS && output_len + finish_len == len) {
        memcpy(data, output, len);
    } else {
        status = PSA_ERROR_GENERIC_ERROR;
    }
    psa_cipher_abort(&operation);
    psa_destroy_key(key_id);
    return status;
}

////////////////////////////////////////////////////////////////////////////////
/// Decrypts in place with an authenticated mode (GCM). The payload is
/// nonce || ciphertext || tag, and the tag is verified: a wrong key or a changed
/// byte fails the call instead of producing garbage plaintext.
///
/// @param data          The nonce, ciphertext and tag; on success it holds the
///                      plaintext.
/// @param len           Total length of data.
/// @param c             The cipher row (algorithm and key).
/// @param plaintext_len Receives how many bytes of data are now plaintext.
/// @return PSA_SUCCESS, or PSA_ERROR_INVALID_SIGNATURE when the tag does not
///         verify.
static psa_status_t decrypt_aead(unsigned char* data,
                                 size_t len,
                                 const cipher_entry_t* c,
                                 size_t* plaintext_len)
{
    if (len <= CRYPTO_AEAD_NONCE_SIZE + CRYPTO_AEAD_TAG_SIZE) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        return status;
    }

    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    status = import_key(c, &key_id);
    if (status == PSA_SUCCESS) {
        status = psa_aead_decrypt(key_id, psa_cipher_algorithm(c), data, CRYPTO_AEAD_NONCE_SIZE,
                                  NULL, 0, data + CRYPTO_AEAD_NONCE_SIZE,
                                  len - CRYPTO_AEAD_NONCE_SIZE, data, len - CRYPTO_AEAD_NONCE_SIZE,
                                  plaintext_len);
    }
    if (key_id != PSA_KEY_ID_NULL) {
        psa_destroy_key(key_id);
    }
    return status;
}

////////////////////////////////////////////////////////////////////////////////
/// Carries out the decryption a "GET with a payload" request asks for, following
/// the row's mode (authenticated, block or feedback), and leaves the plaintext
/// in `data` for the caller to answer with.
///
/// @param c          The cipher row (algorithm, key, mode).
/// @param payload    The hex ciphertext presented by the caller.
/// @param data       Buffer that receives the plaintext.
/// @param output_len Receives how many plaintext bytes there are.
/// @return NULL on success, or a fixed "error:..." string to send back
///         (unsupported cipher, invalid parameters, too large).
static const char* decrypt_error(const cipher_entry_t* c,
                                 const char* payload,
                                 unsigned char* data,
                                 int* output_len)
{
    if (!c->supported) {
        return "error:unsupported-cipher";
    }

    int n = hex_decode(payload, data, CRYPTO_MAX_PAYLOAD);
    if (n <= 0) {
        return "error:invalid-parameters";
    }
    *output_len = n;

    if (strcmp(c->mode_cipher, "cc") == 0 || strcmp(c->mode_cipher, "gc") == 0) {
        size_t plaintext_len = 0;
        psa_status_t status = decrypt_aead(data, (size_t)n, c, &plaintext_len);
        *output_len = (int)plaintext_len;
        if (status == PSA_ERROR_NOT_SUPPORTED) {
            return "error:unsupported-mode";
        }
        if (status == PSA_ERROR_INVALID_SIGNATURE) {
            return "error:authentication-failed";
        }
        return status == PSA_SUCCESS ? NULL : "error:invalid-parameters";
    }

    psa_status_t status = decrypt_block_or_feedback(data, (size_t)n, c);
    if (status == PSA_ERROR_NOT_SUPPORTED) {
        return strcmp(c->name_cipher, "ar") == 0 ? "error:unsupported-cipher"
                                                 : "error:unsupported-mode";
    }
    if (status != PSA_SUCCESS) {
        return "error:invalid-parameters";
    }
    return NULL;
}

////////////////////////////////////////////////////////////////////////////////
/// Lowercase hex, for values that must match the form another service serves
///  (the identification service publishes 'pi' in lowercase).
static void hex_encode_lower(const unsigned char* data, int len, char* out, size_t out_cap)
{
    static const char DIGITS[] = "0123456789abcdef";
    size_t off = 0;
    for (int i = 0; i < len && off + 2 < out_cap; i++) {
        out[off++] = DIGITS[data[i] >> 4];
        out[off++] = DIGITS[data[i] & 0x0f];
    }
    out[off] = '\0';
}

////////////////////////////////////////////////////////////////////////////////
/// Answer a challenge with a proof of this gateway's identity.
///
/// This is the half of the masquerade story that a verifier drives: an external
/// service has the gateway's public ID, sends a challenge, and checks the answer.
/// 11.2.2.1 gives no wire format for that exchange, so the request's payload is
/// the challenge (hex, because POC payloads are text) and the answer carries:
///
///   pi=<64 hex>    the identity being claimed (Table 41 'pi')
///   pk=<130 hex>   the public key that identity names (0x04 || X || Y)
///   sig=<128 hex>  a deterministic ECDSA P-256 signature over the challenge
///
/// The verifier does two things, and BOTH matter:
///
///   1. checks sig against pk -- proves the sender holds the private key;
///   2. checks pk's X against pi -- proves the key is the one *this* identity
///      names. Without step 2 a valid signature from anybody would do, which
///      would be no protection at all (crypto_pi_matches() does this check).
///
/// The public key travels because rebuilding a point from its X alone needs field
/// arithmetic in the verifier; see docs/poc_design.md 5.3.
static void crypto_identity_proof(service_object_t* so, hes_clme_msg_t* out)
{
    char hex[HES_PAYLOAD_MAX];
    hes_strlcpy(hex, sizeof(hex), out->payload);

    unsigned char challenge[CRYPTO_MAX_PAYLOAD];
    int challenge_len = hex_decode(hex, challenge, sizeof(challenge));
    if (challenge_len <= 0) {
        // No challenge, no proof. Guessing at one would defeat the point.
        hes_msg_set_payload_str(out, "error:bad-challenge");
        return;
    }

    unsigned char pub[CRYPTO_IDENTITY_PUB_BYTES];
    unsigned char sig[CRYPTO_IDENTITY_SIG_BYTES];
    if (crypto_service_public_key(so, pub) != 0 ||
        crypto_service_sign(so, challenge, (size_t)challenge_len, sig) != 0) {
        hes_msg_set_payload_str(out, "error:no-identity-key");
        return;
    }

    char pi[65];
    char pk[CRYPTO_IDENTITY_PUB_BYTES * 2 + 1];
    char sig_hex[CRYPTO_IDENTITY_SIG_BYTES * 2 + 1];
    hex_encode_lower(pub + 1, 32, pi, sizeof(pi));  // 'pi' is the X coordinate
    hex_encode(pub, CRYPTO_IDENTITY_PUB_BYTES, pk, sizeof(pk));
    hex_encode(sig, CRYPTO_IDENTITY_SIG_BYTES, sig_hex, sizeof(sig_hex));

    char buf[HES_PAYLOAD_MAX];
    int n = snprintf(buf, sizeof(buf), "pi=%s;pk=%s;sig=%s", pi, pk, sig_hex);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        // Fail loudly rather than shipping a TRUNCATED proof: half a signature
        // is not a weaker proof, it is a broken one, and the verifier would
        // report it as an authentication failure -- pointing at the wrong bug.
        log_error("crypto: the identity proof needs %d bytes, %d available", n, (int)sizeof(buf));
        hes_msg_set_payload_str(out, "error:proof-too-large");
        return;
    }
    hes_msg_set_payload_str(out, buf);
}

////////////////////////////////////////////////////////////////////////////////
/// Serves a GET on any address of this service. What the answer is depends on
/// the row and on whether the request carries a payload:
///
///   - unknown 'ei'                -> "error:unknown-ei";
///   - the identity row, payload   -> a signature over the presented challenge;
///   - the identity row, no payload-> its capability report;
///   - a cipher row, payload       -> the decrypted plaintext, as hex;
///   - a cipher row, no payload    -> that cipher's own details (ei/nc/ks/mc).
///
/// On entry, out->query and out->payload hold the incoming request (the
/// dispatcher copies them in); the answer overwrites out->payload.
///
/// @param so  The service object, which carries the cipher table.
/// @param out Carries the request in and the reply out.
static void crypto_on_get(service_object_t* so, hes_clme_msg_t* out)
{
    crypto_service_state_t* st = (crypto_service_state_t*)so->state;

    // Per main.c's dispatch convention: on entry, out->query and out->payload
    // already hold the INCOMING request's query/payload (main.c copies them in
    // before calling on_get); this function overwrites out->payload with the
    // response.
    int ei = parse_ei(out->query);
    const cipher_entry_t* c = (ei >= 0) ? find_cipher(st, ei) : NULL;

    if (!c) {
        hes_msg_set_payload_str(out, "error:unknown-ei");
        return;
    }

    // The identity-signature row is not a cipher, so the "GET with a payload"
    // case below (which would decrypt) must not swallow it: with a payload it
    // signs a challenge, and without one it falls through to the capability
    // report, exactly like every other row.
    if (c->internal_bits > 0) {
        if (out->payload_len > 0) {
            crypto_identity_proof(so, out);
        } else {
            char buf[128];
            snprintf(buf, sizeof(buf), "ei=%d;ks=%d;it=%d;au=%d", c->ei, c->key_size,
                     c->internal_bits, c->auth_bytes);
            hes_msg_set_payload_str(out, buf);
        }
        return;
    }

    if (out->payload_len > 0) {
        unsigned char raw[CRYPTO_MAX_PAYLOAD];
        int n = 0;
        const char* error = decrypt_error(c, out->payload, raw, &n);
        if (error) {
            hes_msg_set_payload_str(out, error);
            return;
        }
        char hex[257];
        hex_encode(raw, n, hex, sizeof(hex));
        hes_msg_set_payload_str(out, hex);
    } else {
        // One 'transCode=value' record per field, ';' between records -- the
        // convention every other service object uses (see services/id/id.h).
        // The row used to be comma-separated, which quietly made it the one
        // object whose data could not be picked apart with the router's datum
        // addressing ('?da=ks' or a trailing transCode), since the router
        // splits records on ';'.
        //
        // Only CIPHER rows reach here: an internal row was answered above.
        char buf[128];
        snprintf(buf, sizeof(buf), "ei=%d;nc=%s;ks=%d;mc=%s", c->ei, c->name_cipher, c->key_size,
                 c->mode_cipher);
        hes_msg_set_payload_str(out, buf);
    }
}

////////////////////////////////////////////////////////////////////////////////
/// Refuses a PUT. This POC has no key installation or rotation, so there is
/// nothing to apply; a real implementation would accept key commands here.
///
/// @param so The service object (unused).
/// @param in The PUT being ignored.
static void crypto_on_put(service_object_t* so, const hes_clme_msg_t* in)
{
    // A real implementation would accept key install/rotation commands here.
    // Not needed for this POC.
    (void)so;
    (void)in;
}

////////////////////////////////////////////////////////////////////////////////
/// Releases the service state. One state is shared by the objects at both
/// addresses, so only the last object out scrubs the secret and frees it.
///
/// @param so The service object being destroyed.
static void crypto_destroy(service_object_t* so)
{
    crypto_service_state_t* st = (crypto_service_state_t*)so->state;
    if (!st) {
        return;
    }
    so->state = NULL;

    // Shared by the objects at both addresses, so only the last one out does
    // the scrubbing.
    if (--st->refs > 0) {
        return;
    }

    // The identity scalar is a secret, so scrub it rather than merely dropping
    // the allocation -- the same treatment the identification service gives
    // 'fp'. Written through a volatile pointer so the compiler cannot optimise
    // the erase away.
    volatile unsigned char* p = (volatile unsigned char*)st->identity_scalar;
    for (size_t i = 0; i < sizeof(st->identity_scalar); i++) {
        p[i] = 0;
    }
    free(st);
}

////////////////////////////////////////////////////////////////////////////////
// The identity key pair: sign, verify, pin
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// SHA-256 of a message, as a 32-byte digest.
static int sha256_digest(const unsigned char* msg, size_t msg_len, unsigned char out[32])
{
    size_t out_len = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, msg, msg_len, out, 32, &out_len) != PSA_SUCCESS) {
        return -1;
    }
    return out_len == 32 ? 0 : -1;
}

////////////////////////////////////////////////////////////////////////////////
/// Reads one 'code=value' record out of a credential string.
///
/// @param cred The message's 'ci'.
/// @param code Record name, e.g. "pi".
/// @param out Receives the value, NUL-terminated, or "" when absent.
/// @param out_cap Room in 'out'.
/// @return 1 when the record was found and fit.
static int proof_field(const char* cred, const char* code, char* out, size_t out_cap)
{
    out[0] = '\0';
    if (!cred) {
        return 0;
    }

    size_t clen = strlen(code);
    const char* p = cred;
    while (*p != '\0') {
        const char* end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        const char* eq = memchr(p, '=', len);

        if (eq != NULL && (size_t)(eq - p) == clen && strncmp(p, code, clen) == 0) {
            size_t vlen = len - clen - 1;
            if (vlen >= out_cap) {
                return 0;
            }
            memcpy(out, eq + 1, vlen);
            out[vlen] = '\0';
            return 1;
        }
        if (end == NULL) {
            break;
        }
        p = end + 1;
    }
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Tables 70/71 -- the capability declaration, memoryType 'pr'.
///
/// All three values are fixed by 11.2.5.2, so they are constants here rather
/// than configuration anyone could get wrong: 'vr' is the service version,
/// 'rq' is 'ma' (a cryptographic service is mandatory), and 'si' is 'no' --
/// this service does NOT require a single instance, unlike the identification
/// service, whose 'si' is 'ye'.
static void crypto_config_on_get(service_object_t* so, hes_clme_msg_t* out)
{
    (void)so;
    hes_msg_set_payload_str(out, "vr=1.0.0;rq=ma;si=no");
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

void crypto_service_set_identity_key(service_object_t* so,
                                     const unsigned char priv[CRYPTO_IDENTITY_KEY_BYTES])
{
    if (!so || !so->state || !priv) {
        return;
    }
    crypto_service_state_t* st = (crypto_service_state_t*)so->state;
    memcpy(st->identity_scalar, priv, CRYPTO_IDENTITY_KEY_BYTES);
    st->have_identity_key = 1;
}

int crypto_service_sign(service_object_t* so,
                        const unsigned char* msg,
                        size_t msg_len,
                        unsigned char sig[CRYPTO_IDENTITY_SIG_BYTES])
{
    if (!so || !so->state || (!msg && msg_len)) {
        return -1;
    }
    crypto_service_state_t* st = (crypto_service_state_t*)so->state;
    if (!st->have_identity_key) {
        log_error("crypto: no identity key installed, so nothing can be signed");
        return -1;
    }

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        log_error("crypto: PSA crypto init failed (0x%04x)", (unsigned)status);
        return -1;
    }

    unsigned char digest[32];
    if (sha256_digest(msg, msg_len, digest) != 0) {
        log_error("crypto: cannot hash the message to sign");
        return -1;
    }

    // Import the scalar for the duration of one signature and drop it again, so
    // the key exists in PSA's key store for as short a time as possible.
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(&attr, PSA_ALG_DETERMINISTIC_ECDSA(PSA_ALG_SHA_256));

    psa_key_id_t key = PSA_KEY_ID_NULL;
    status = psa_import_key(&attr, st->identity_scalar, CRYPTO_IDENTITY_KEY_BYTES, &key);
    psa_reset_key_attributes(&attr);
    if (status != PSA_SUCCESS) {
        log_error("crypto: cannot import the identity key (0x%04x)", (unsigned)status);
        return -1;
    }

    size_t sig_len = 0;
    status = psa_sign_hash(key, PSA_ALG_DETERMINISTIC_ECDSA(PSA_ALG_SHA_256), digest,
                           sizeof(digest), sig, CRYPTO_IDENTITY_SIG_BYTES, &sig_len);
    psa_destroy_key(key);

    if (status != PSA_SUCCESS || sig_len != CRYPTO_IDENTITY_SIG_BYTES) {
        log_error("crypto: signing failed (0x%04x)", (unsigned)status);
        return -1;
    }
    return 0;
}

int crypto_service_public_key(service_object_t* so, unsigned char pub[CRYPTO_IDENTITY_PUB_BYTES])
{
    if (!so || !so->state || !pub) {
        return -1;
    }
    crypto_service_state_t* st = (crypto_service_state_t*)so->state;
    if (!st->have_identity_key) {
        log_error("crypto: no identity key installed, so there is no public key to give");
        return -1;
    }

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        return -1;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attr, PSA_ALG_DETERMINISTIC_ECDSA(PSA_ALG_SHA_256));

    psa_key_id_t key = PSA_KEY_ID_NULL;
    status = psa_import_key(&attr, st->identity_scalar, CRYPTO_IDENTITY_KEY_BYTES, &key);
    psa_reset_key_attributes(&attr);
    if (status != PSA_SUCCESS) {
        return -1;
    }

    size_t pub_len = 0;
    status = psa_export_public_key(key, pub, CRYPTO_IDENTITY_PUB_BYTES, &pub_len);
    psa_destroy_key(key);

    if (status != PSA_SUCCESS || pub_len != CRYPTO_IDENTITY_PUB_BYTES) {
        log_error("crypto: cannot export the identity public key (0x%04x)", (unsigned)status);
        return -1;
    }
    return 0;
}

int crypto_verify_message(const unsigned char pub[CRYPTO_IDENTITY_PUB_BYTES],
                          const unsigned char* msg,
                          size_t msg_len,
                          const unsigned char sig[CRYPTO_IDENTITY_SIG_BYTES])
{
    if (!pub || !sig || (!msg && msg_len)) {
        return -1;
    }
    if (pub[0] != 0x04) {
        log_error("crypto: presented public key is not an uncompressed P-256 point");
        return -1;
    }

    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        return -1;
    }

    unsigned char digest[32];
    if (sha256_digest(msg, msg_len, digest) != 0) {
        return -1;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attr, PSA_ALG_DETERMINISTIC_ECDSA(PSA_ALG_SHA_256));

    psa_key_id_t key = PSA_KEY_ID_NULL;
    status = psa_import_key(&attr, pub, CRYPTO_IDENTITY_PUB_BYTES, &key);
    psa_reset_key_attributes(&attr);
    if (status != PSA_SUCCESS) {
        return -1;
    }

    status = psa_verify_hash(key, PSA_ALG_DETERMINISTIC_ECDSA(PSA_ALG_SHA_256), digest,
                             sizeof(digest), sig, CRYPTO_IDENTITY_SIG_BYTES);
    psa_destroy_key(key);

    if (status != PSA_SUCCESS) {
        // Not an error worth shouting about: a failed proof is the expected
        // outcome of an impostor trying, so this is informational.
        log_info("crypto: signature did not verify against the presented key");
        return -1;
    }
    return 0;
}

int crypto_pi_matches(const unsigned char pub[CRYPTO_IDENTITY_PUB_BYTES], const char* pi)
{
    if (!pub || !pi || pub[0] != 0x04 || strlen(pi) != 64) {
        return 0;
    }

    // Compare the X coordinate nibble by nibble, case-insensitively, so an
    // upper-case 'pi' from a document still matches.
    static const char DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        char hi = DIGITS[pub[1 + i] >> 4];
        char lo = DIGITS[pub[1 + i] & 0x0f];
        if (hi != (char)tolower((unsigned char)pi[2 * i]) ||
            lo != (char)tolower((unsigned char)pi[2 * i + 1])) {
            return 0;
        }
    }
    return 1;
}

////////////////////////////////////////////////////////////////////////////////
// Verifying a proof presented by a peer
////////////////////////////////////////////////////////////////////////////////

size_t crypto_proof_message(const char* path, const char* payload, char* out, size_t out_cap)
{
    if (!out || out_cap == 0) {
        return 0;
    }
    out[0] = '\0';
    size_t used = hes_strlcpy(out, out_cap, path ? path : "");
    if (used + 1 >= out_cap) {
        return used;
    }
    out[used++] = '\n';
    out[used] = '\0';
    used += hes_strlcpy(out + used, out_cap - used, payload ? payload : "");
    return used;
}

int crypto_verify_proof(const char* cred_info,
                        const char* expected_pi,
                        const unsigned char* msg,
                        size_t msg_len)
{
    char pi[65];
    char pk_hex[CRYPTO_IDENTITY_PUB_BYTES * 2 + 1];
    char sig_hex[CRYPTO_IDENTITY_SIG_BYTES * 2 + 1];

    if (!proof_field(cred_info, "pi", pi, sizeof(pi)) ||
        !proof_field(cred_info, "pk", pk_hex, sizeof(pk_hex)) ||
        !proof_field(cred_info, "sig", sig_hex, sizeof(sig_hex))) {
        // No proof at all is the interesting case, and it is a denial, not an
        // error: an operation that demands a proof and does not get one must
        // not proceed.
        log_info("crypto: no complete proof (pi/pk/sig) presented");
        return 0;
    }

    unsigned char pub[CRYPTO_IDENTITY_PUB_BYTES];
    unsigned char sig[CRYPTO_IDENTITY_SIG_BYTES];
    if (hex_decode(pk_hex, pub, sizeof(pub)) != CRYPTO_IDENTITY_PUB_BYTES ||
        hex_decode(sig_hex, sig, sizeof(sig)) != CRYPTO_IDENTITY_SIG_BYTES) {
        log_info("crypto: presented proof is malformed");
        return 0;
    }

    // Pin the key to the identity the policy expects BEFORE verifying, so a
    // proof from the wrong gateway cannot even reach the maths.
    if (!crypto_pi_matches(pub, expected_pi)) {
        log_info("crypto: presented proof is not for the expected identity");
        return 0;
    }
    if (!crypto_pi_matches(pub, pi)) {
        log_info("crypto: presented proof claims a different identity than its own key");
        return 0;
    }
    if (crypto_verify_message(pub, msg, msg_len, sig) != 0) {
        return 0;
    }

    return 1;
}

int crypto_service_create(service_object_t** out, int max_out)
{
    if (!out || max_out < CRYPTO_SERVICE_OBJECT_COUNT) {
        log_error("crypto: need room for %d service objects", CRYPTO_SERVICE_OBJECT_COUNT);
        return -1;
    }

    crypto_service_state_t* st = calloc(1, sizeof(*st));
    if (!st) {
        log_error("crypto: out of memory");
        return -1;
    }

    cipher_entry_t* c = &st->ciphers[0];
    const char* definitions[][3] = {
            {"ae", "cb", "1"}, {"ae", "cf", "1"}, {"ae", "cc", "1"}, {"ae", "gc", "1"},
            {"ar", "cb", "1"}, {"ar", "cf", "1"}, {"ar", "cc", "1"}, {"ar", "gc", "1"},
            {"bf", "cb", "0"}, {"bf", "cc", "0"}, {"bf", "cf", "0"}, {"bf", "gc", "0"},
    };
    for (size_t i = 0; i < sizeof(definitions) / sizeof(definitions[0]); i++) {
        c = &st->ciphers[st->n_ciphers++];
        c->ei = (int)st->n_ciphers;
        strncpy(c->name_cipher, definitions[i][0], sizeof(c->name_cipher) - 1);
        strncpy(c->mode_cipher, definitions[i][1], sizeof(c->mode_cipher) - 1);
        c->key_size = 16;
        c->supported = definitions[i][2][0] == '1';
        memcpy(c->key, "0123456789ABCDEF", 16);
    }

    // The identity-signature operation. It is not a cipher, so it carries no
    // 'nc'/'mc'; Table 73's 'it' (internal operations, bits) marks it as an
    // internal operation and 'au' gives the authorization size. 256 bits is the
    // P-256 group order, and 64 bytes is the size of the signature that comes
    // back (r || s). The key size is stated so 'ks' means "a key is installed"
    // rather than its "0: key not installed" default.
    c = &st->ciphers[st->n_ciphers++];
    c->ei = CRYPTO_IDENTITY_EI;
    c->key_size = CRYPTO_IDENTITY_KEY_BYTES;
    c->internal_bits = 256;
    c->auth_bytes = CRYPTO_IDENTITY_SIG_BYTES;
    c->supported = 1;

    // One service object per Lexicon address: same service, two doors.
    static const struct crypto_address {
        const char* path;
        void (*on_get)(service_object_t*, hes_clme_msg_t*);
    } ADDRESSES[CRYPTO_SERVICE_OBJECT_COUNT] = {
            {HES_LX_CRYPTO, crypto_config_on_get},  // Tables 70/71
            {HES_LX_CRYPTO_CI, crypto_on_get},      // Tables 72/73
    };

    for (int i = 0; i < CRYPTO_SERVICE_OBJECT_COUNT; i++) {
        service_object_t* so = calloc(1, sizeof(*so));
        if (!so) {
            for (int j = 0; j < i; j++) {
                out[j]->destroy(out[j]);
                free(out[j]);
                out[j] = NULL;
            }
            volatile unsigned char* p = (volatile unsigned char*)st->identity_scalar;
            for (size_t k = 0; k < sizeof(st->identity_scalar); k++) {
                p[k] = 0;
            }
            free(st);
            log_error("crypto: out of memory");
            return -1;
        }

        hes_strlcpy(so->path, sizeof(so->path), ADDRESSES[i].path);
        so->state = st;
        so->on_get = ADDRESSES[i].on_get;
        so->on_put = crypto_on_put;
        so->tick = NULL;  // no autonomous behaviour
        so->destroy = crypto_destroy;
        st->refs++;
        out[i] = so;
    }
    return CRYPTO_SERVICE_OBJECT_COUNT;
}
