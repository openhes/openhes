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
/// @brief Implementation of the identification service (id.h): the identity
/// document, the deterministic 'fp' -> 'pi' derivation, and the objects served.
///
/// @details
/// The service owns the gateway's identity pair and the members that present it:
///
///   - the load path: the identity document is read once and validated field by
///     field (64 hex characters each, well-formed, and the optional public 'pi'
///     checked against the one derived from 'fp'), then its key material imported.
///
///   - the derivation: 'fp' -> HKDF-SHA256 -> a P-256 scalar -> the public key, so
///     'pi' is stationary for as long as 'fp' is and needs no separate storage.
///
///   - the members: version, system number, description, class, uniquePublicID and
///     the capability declaration, each answering a GET from the values above.
///     'fp' itself is never served.

#include "id.h"

#include <jansson.h>
#include <log.h>
#include <psa/crypto.h>

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// 'pi' (uniquePublicID) and 'fp' (digitalFingerprint) are 256-bit values
/// (11.2.2.1). We carry them as 64 lowercase hex characters, which is one legal
/// encoding -- the standard's dataFormat for both fields is simply 'string'.
/// The 256 bits constrain how the value is *generated* (true random), not how
/// it is written down.
#define ID_HEX_CHARS 64
#define ID_VALUE_MAX (ID_HEX_CHARS + 1)

#define ID_VERSION_MAX 16  ///< 'vr'
#define ID_SYSNUM_MAX 16   ///< 'hs'
#define ID_DESC_MAX 64     ///< 'pd'
#define ID_CLASS_MAX 8     ///< 'ch'

#define ID_DEFAULT_VERSION "1.0.0"
#define ID_DEFAULT_SYSNUM "1"
#define ID_DEFAULT_CLASS "sh"  ///< simple gateway (HAN-HAN), ISO/IEC 15045-4-1

typedef struct id_service_state {
    /// One state, shared by the objects at every address, so it is reference
    /// counted: the last object destroyed scrubs the secret.
    int refs;

    /// Table 41 Data, memoryType 'ro' -- viewable, preset.
    char unique_public_id[ID_VALUE_MAX];     ///< 'pi': public and STATIONARY
    char digital_fingerprint[ID_VALUE_MAX];  ///< 'fp': SECRET, never served
    char hes_clme_ver[ID_VERSION_MAX];       ///< 'vr'
    char hes_system_number[ID_SYSNUM_MAX];   ///< 'hs'
    char public_description[ID_DESC_MAX];    ///< 'pd'
    char gateway_class[ID_CLASS_MAX];        ///< 'ch'

    /// Table 44 metaData, memoryType 'po' (post-market): the installer's risk
    /// assessment of HES-CLME for this installation. 0 = not initialized.
    long vulnerability;   ///< 'vl'
    long adverse_impact;  ///< 'il'
    long likelihood;      ///< 'll'
    long risk_level;      ///< 'rl'

    /// The private half of the identity key pair derived from 'fp' (see the
    /// header). Handed to the crypto service once at startup and then wiped, so
    /// the secret lives in exactly one place -- the component that uses it.
    unsigned char identity_scalar[ID_IDENTITY_KEY_BYTES];
    int have_identity_key;  ///< 0 = already handed over, or never derived

    /// Tables 39/42 -- gateway inventory the host answers; NULL means the
    /// service genuinely does not know, and reports zeros.
    id_inventory_fn inventory;
    void* inventory_ctx;
} id_service_state_t;

////////////////////////////////////////////////////////////////////////////////
/// Best-effort scrub. Writes through a volatile pointer so the compiler cannot
/// optimise the erase away, which keeps the fingerprint from lingering in freed
/// heap memory.
static void secure_wipe(void* p, size_t n)
{
    volatile unsigned char* v = (volatile unsigned char*)p;
    while (n-- > 0) {
        *v++ = 0;
    }
}

////////////////////////////////////////////////////////////////////////////////
/// Copies a NUL-terminated string, treating NULL as "nothing to copy".
static void copy_str(char* dst, size_t cap, const char* src)
{
    hes_strlcpy(dst, cap, src ? src : "");
}

////////////////////////////////////////////////////////////////////////////////
/// Validates one 256-bit identity value: exactly 64 hex characters, nothing
/// else, normalised to lowercase so the two encodings of one value compare
/// equal. Returns 0 on success, -1 when the value is missing or malformed.
///
/// The rejected value itself is never logged: 'fp' is memoryType 'sp'
/// (obscured), so even a malformed fingerprint must not reach a log file.
static int take_hex256(const char* trans_code, const char* value, char* out, size_t out_cap)
{
    if (!value) {
        log_error("id: identity document is missing '%s'", trans_code);
        return -1;
    }
    if (strlen(value) != ID_HEX_CHARS || out_cap < ID_VALUE_MAX) {
        log_error("id: '%s' must be exactly %d hex characters", trans_code, ID_HEX_CHARS);
        return -1;
    }
    for (size_t i = 0; i < ID_HEX_CHARS; i++) {
        if (!isxdigit((unsigned char)value[i])) {
            log_error("id: '%s' contains a non-hex character", trans_code);
            return -1;
        }
        out[i] = (char)tolower((unsigned char)value[i]);
    }
    out[ID_HEX_CHARS] = '\0';
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Reads an optional string field of the identity document, with a fallback.
static void take_optional(json_t* gw, const char* key, char* out, size_t cap, const char* fallback)
{
    json_t* v = json_object_get(gw, key);
    copy_str(out, cap, json_is_string(v) ? json_string_value(v) : fallback);
}

////////////////////////////////////////////////////////////////////////////////
/// Reads an optional non-negative integer field, defaulting to 0 (= "not
/// initialized", Table 44).
static void take_optional_int(json_t* gw, const char* key, long* out)
{
    json_t* v = json_object_get(gw, key);
    *out = (json_is_integer(v) && json_integer_value(v) > 0) ? (long)json_integer_value(v) : 0;
}

////////////////////////////////////////////////////////////////////////////////
// The identity key pair: 'fp' -> a P-256 key pair -> 'pi'
////////////////////////////////////////////////////////////////////////////////

/// HKDF info string. Domain separation: if 'fp' is ever used for a second
/// purpose, that purpose must use a different info string, so the two derived
/// values cannot be related to each other.
#define ID_KEY_INFO "openhes-id-p256-v1"

/// How many HKDF counters to try. PSA rejects a scalar that is zero or at or
/// above the group order -- odds of about 2^-32 -- so one retry is already
/// generous.
#define ID_KEY_TRIES 4

// Order n of the P-256 group, big-endian. Needed only to negate a scalar, which
// is a plain bignum subtraction, so no bignum library is dragged in for it.
static const unsigned char P256_ORDER[32] = {
        0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17,
        0x9e, 0x84, 0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x51,
};

////////////////////////////////////////////////////////////////////////////////
/// Value of one hexadecimal digit.
///
/// @param c The character, e.g. '7', 'a' or 'F'.
/// @return 0-15, or -1 when the character is not a hex digit.
static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

////////////////////////////////////////////////////////////////////////////////
/// Decodes n bytes from a 2n-character hex string. Both cases are accepted,
/// since an identity document may be written either way.
///
/// @param hex The hex text, e.g. a 64-character 'pi' or 'fp'.
/// @param out Receives the n decoded bytes.
/// @param n   How many bytes to decode.
/// @return 0 on success, -1 when any character is not a hex digit.
static int hex_to_bytes(const char* hex, unsigned char* out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int hi = hex_val(hex[2 * i]);
        int lo = hex_val(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Encodes n bytes as 2n lower-case hex characters and terminates them, so the
/// caller's buffer must hold 2n + 1 bytes.
///
/// @param in  The bytes to encode, e.g. the X coordinate of 'pi'.
/// @param n   How many bytes to encode.
/// @param out Receives the hex text.
static void bytes_to_hex(const unsigned char* in, size_t n, char* out)
{
    static const char DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = DIGITS[in[i] >> 4];
        out[2 * i + 1] = DIGITS[in[i] & 0x0f];
    }
    out[2 * n] = '\0';
}

////////////////////////////////////////////////////////////////////////////////
/// out = n - d (big-endian, 32 bytes): the scalar whose public point has Y
///  negated and the same X.
static void scalar_negate(const unsigned char d[ID_IDENTITY_KEY_BYTES],
                          unsigned char out[ID_IDENTITY_KEY_BYTES])
{
    unsigned borrow = 0;
    for (int i = ID_IDENTITY_KEY_BYTES - 1; i >= 0; i--) {
        int t = (int)P256_ORDER[i] - (int)d[i] - (int)borrow;
        if (t < 0) {
            t += 0x100;
            borrow = 1;
        } else {
            borrow = 0;
        }
        out[i] = (unsigned char)t;
    }
}

/// HKDF salt. Fixed and public on purpose: HKDF's salt is not a secret -- it
/// exists to separate this use of the key material from any other -- and a fixed
/// one keeps the derivation reproducible, which is exactly what makes 'pi'
/// stationary. It is also non-empty, because an empty salt is the kind of edge
/// case implementations differ on.
static const unsigned char ID_KEY_SALT[] = "openhes-id-v1";

////////////////////////////////////////////////////////////////////////////////
/// HKDF-SHA256 expansion of 'ikm' under 'info', one output block.
static int hkdf_sha256(const unsigned char* ikm,
                       size_t ikm_len,
                       const char* info,
                       unsigned char* out,
                       size_t out_len)
{
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        log_error("id: PSA crypto init failed (0x%04x)", (unsigned)status);
        return -1;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_DERIVE);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
    // The policy must NAME the algorithm: PSA checks the requested algorithm
    // against the key's policy, and a policy algorithm of PSA_ALG_NONE permits
    // nothing at all. Leaving this out is what makes
    // psa_key_derivation_input_key fail with NOT_PERMITTED.
    psa_set_key_algorithm(&attr, PSA_ALG_HKDF(PSA_ALG_SHA_256));

    psa_key_id_t key = PSA_KEY_ID_NULL;
    status = psa_import_key(&attr, ikm, ikm_len, &key);
    psa_reset_key_attributes(&attr);
    if (status != PSA_SUCCESS) {
        log_error("id: cannot import the key material for HKDF (0x%04x)", (unsigned)status);
        return -1;
    }

    // The input order is fixed by HKDF: salt, then secret, then info.
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    status = psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256));
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, ID_KEY_SALT,
                                                sizeof(ID_KEY_SALT) - 1);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_key(&op, PSA_KEY_DERIVATION_INPUT_SECRET, key);
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO,
                                                (const unsigned char*)info, strlen(info));
    }
    if (status == PSA_SUCCESS) {
        status = psa_key_derivation_output_bytes(&op, out, out_len);
    }

    psa_key_derivation_abort(&op);
    psa_destroy_key(key);

    if (status != PSA_SUCCESS) {
        log_error("id: HKDF-SHA256 failed (0x%04x)", (unsigned)status);
        return -1;
    }
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Import a candidate P-256 scalar and export its uncompressed public point.
///
/// @return 0 when 'candidate' is a usable scalar (PSA rejects zero and anything
///         at or above the group order), with 'pub' filled in and 'accepted'
///         holding the scalar PSA actually took.
static int public_from_scalar(const unsigned char candidate[ID_IDENTITY_KEY_BYTES],
                              unsigned char pub[65],
                              unsigned char accepted[ID_IDENTITY_KEY_BYTES])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(&attr, PSA_ALG_DETERMINISTIC_ECDSA(PSA_ALG_SHA_256));

    psa_key_id_t key = PSA_KEY_ID_NULL;
    psa_status_t status = psa_import_key(&attr, candidate, ID_IDENTITY_KEY_BYTES, &key);
    psa_reset_key_attributes(&attr);
    if (status != PSA_SUCCESS) {
        // Expected occasionally -- PSA rejects a scalar of zero or one at or
        // above the group order -- but rare enough (about 2^-32) that a repeat
        // here means the key policy above is wrong, so say which it was.
        log_error("id: cannot import the candidate identity scalar (0x%04x)", (unsigned)status);
        return -1;
    }

    size_t pub_len = 0;
    status = psa_export_public_key(key, pub, 65, &pub_len);
    psa_destroy_key(key);
    if (status != PSA_SUCCESS) {
        log_error("id: cannot export the identity public key (0x%04x)", (unsigned)status);
        return -1;
    }
    if (pub_len != 65 || pub[0] != 0x04) {
        log_error("id: identity public key is not a 65-byte uncompressed point (%u bytes)",
                  (unsigned)pub_len);
        return -1;
    }

    memcpy(accepted, candidate, ID_IDENTITY_KEY_BYTES);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Derive the identity key pair from the fingerprint, and publish its public
/// half as 'pi'.
///
/// Deterministic, which is what keeps 'pi' stationary for as long as 'fp' is:
/// there is no key file to store or back up, and the same 'fp' gives the same
/// 'pi' across restarts and reinstalls.
///
/// @return 0 on success, -1 when PSA cannot do the job at all.
static int derive_identity_key(id_service_state_t* st)
{
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        log_error("id: PSA crypto init failed (0x%04x)", (unsigned)status);
        return -1;
    }

    unsigned char fp[ID_IDENTITY_KEY_BYTES];
    if (hex_to_bytes(st->digital_fingerprint, fp, sizeof(fp)) != 0) {
        log_error("id: 'fp' is not usable as key material");
        return -1;
    }

    unsigned char pub[65];
    int derived = 0;
    for (int attempt = 0; attempt < ID_KEY_TRIES && !derived; attempt++) {
        // The counter keeps a rejected scalar from re-deriving the same bytes.
        char info[64];
        snprintf(info, sizeof(info), ID_KEY_INFO "/%d", attempt);

        unsigned char candidate[ID_IDENTITY_KEY_BYTES];
        if (hkdf_sha256(fp, sizeof(fp), info, candidate, sizeof(candidate)) != 0) {
            break;
        }

        if (public_from_scalar(candidate, pub, st->identity_scalar) == 0) {
            derived = 1;
        }
    }
    secure_wipe(fp, sizeof(fp));

    if (!derived) {
        log_error("id: cannot derive an identity key from 'fp'");
        return -1;
    }

    // Canonicalise. 'pi' is the X coordinate alone, so which of the two points
    // sharing that X is meant has to be pinned down by a rule: Y is required to
    // be the EVEN root. If this candidate sits on the odd one, negating the
    // scalar gives the same X with Y negated -- same key material, canonical
    // encoding, and a verifier who knows the rule can rebuild the point from
    // 'pi' with no extra hint.
    if ((pub[64] & 1) != 0) {
        unsigned char negated[ID_IDENTITY_KEY_BYTES];
        scalar_negate(st->identity_scalar, negated);
        if (public_from_scalar(negated, pub, st->identity_scalar) != 0 || (pub[64] & 1) != 0) {
            log_error("id: cannot canonicalise the identity key");
            return -1;
        }
    }

    // 'pi' = the X coordinate: 64 hex characters, exactly the width 11.2.2.1
    // gives the field.
    bytes_to_hex(pub + 1, ID_IDENTITY_KEY_BYTES, st->unique_public_id);
    st->have_identity_key = 1;
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Does the document's optional 'pi' agree with the one derived from 'fp'?
///
/// Absent is fine -- the derived value is logged so it can be pasted in. But a
/// "pi" that is *present* and disagrees would advertise a key that does not
/// match 'fp', and every challenge against it would fail with no visible
/// reason, so that is a startup failure rather than a warning.
///
/// @return 1 when acceptable, 0 when it disagrees.
static int document_pi_matches(const char* path, const char* pi, const char* derived)
{
    if (pi == NULL) {
        return 1;
    }

    char given[ID_VALUE_MAX];
    if (take_hex256("pi", pi, given, sizeof(given)) != 0) {
        return 0;
    }

    if (strcmp(given, derived) != 0) {
        log_error("id: 'pi' in '%s' does not match the public key derived from 'fp'", path);
        log_error("id: derived pi=%s -- paste that in, or drop 'pi' from the document", derived);
        return 0;
    }
    return 1;
}

////////////////////////////////////////////////////////////////////////////////
/// Loads and validates the identity document.
///
/// Fails closed: a missing or malformed field is an error, because a gateway
/// that cannot present a well-formed identity must not join the network --
/// and, more importantly, must not silently substitute a fresh random ID for
/// the provisioned one (11.2.2.1 requires the public ID to be *stationary*).
///
/// Where a product keeps these values, and why they are split across two
/// memoryTypes, is explained in README.md.
static int load_identity(id_service_state_t* st, const char* path)
{
    json_error_t err;
    json_t* root = json_load_file(path, 0, &err);
    if (!root) {
        // Deliberately not logging err.text: on a malformed document jansson can
        // quote the offending line, which may well be the secret. A negative
        // line number means the file could not be opened at all.
        if (err.line < 0) {
            log_error("id: cannot read identity document '%s'", path);
        } else {
            log_error("id: cannot parse identity document '%s' (line %d)", path, err.line);
        }
        return -1;
    }

    json_t* gw = json_object_get(root, "gatewayIdentity");
    if (!json_is_object(gw)) {
        log_error("id: '%s' must contain a 'gatewayIdentity' object", path);
        json_decref(root);
        return -1;
    }

    json_t* jpi = json_object_get(gw, "pi");
    json_t* jfp = json_object_get(gw, "fp");
    const char* pi = json_is_string(jpi) ? json_string_value(jpi) : NULL;
    const char* fp = json_is_string(jfp) ? json_string_value(jfp) : NULL;

    int rc = 0;
    // 'fp' is the ROOT of the identity, not a sibling of 'pi': everything else,
    // 'pi' included, is derived from it. It is therefore the one field the
    // document must carry.
    if (take_hex256("fp", fp, st->digital_fingerprint, sizeof(st->digital_fingerprint)) != 0) {
        rc = -1;
    } else if (derive_identity_key(st) != 0) {
        rc = -1;
    } else if (!document_pi_matches(path, pi, st->unique_public_id)) {
        rc = -1;
    } else {
        // 'pi' is public -- it is served on HES-CLME -- so printing it is safe,
        // and useful: it is the value an operator pastes into the document and
        // the value an external service pins. 'fp' is never logged.
        log_info("id: identity key derived; pi=%s", st->unique_public_id);
        take_optional(gw, "vr", st->hes_clme_ver, sizeof(st->hes_clme_ver), ID_DEFAULT_VERSION);
        take_optional(gw, "hs", st->hes_system_number, sizeof(st->hes_system_number),
                      ID_DEFAULT_SYSNUM);
        take_optional(gw, "pd", st->public_description, sizeof(st->public_description), "");
        take_optional(gw, "ch", st->gateway_class, sizeof(st->gateway_class), ID_DEFAULT_CLASS);

        // Table 44 metaData: installer-provisioned risk assessment.
        take_optional_int(gw, "vl", &st->vulnerability);
        take_optional_int(gw, "il", &st->adverse_impact);
        take_optional_int(gw, "ll", &st->likelihood);
        take_optional_int(gw, "rl", &st->risk_level);
    }
    json_decref(root);
    return rc;
}

////////////////////////////////////////////////////////////////////////////////
/// Table 37 configurationData, memoryType 'pr' (ROM-pre, installer
/// provisioned): the capability declaration.
///
/// 'ma' (mandatory) and 'ye' (single instance) are the standard's *preassigned*
/// values, not configuration, so they are constants here rather than something
/// the identity document can get wrong.
static void id_config_on_get(service_object_t* so, hes_clme_msg_t* out)
{
    (void)so;
    hes_msg_set_payload_str(out, "rq=ma;si=ye");
}

////////////////////////////////////////////////////////////////////////////////
/// Tables 41/42/43/44 -- every data kind that shares the centralOperations
/// address, in one answer.
///
/// 'fp' (Table 43) is deliberately absent. Its memoryType is 'sp' -- obscured,
/// i.e. not readable over HES-CLME "without proper keys" -- so the fingerprint
/// never leaves this process. This is the object's core promise; see README.md
/// ("Why 'fp' is never in a payload").
///
/// The counts (nh/nw/ns) and the risk levels (vl/il/ll/rl) are asked for on
/// every read rather than cached: the standard calls the counts operational data
/// ("from last discovery process"), so they may change under us.
static void id_status_on_get(service_object_t* so, hes_clme_msg_t* out)
{
    id_service_state_t* st = (id_service_state_t*)so->state;
    id_inventory_t inv;
    char buf[HES_PAYLOAD_MAX];

    memset(&inv, 0, sizeof(inv));
    if (st->inventory) {
        st->inventory(st->inventory_ctx, &inv);
    }

    int n = snprintf(buf, sizeof(buf),
                     "pi=%s;vr=%s;hs=%s;ch=%s;pd=%s;nh=%d;nw=%d;ns=%d;"
                     "vl=%ld;il=%ld;ll=%ld;rl=%ld",
                     st->unique_public_id, st->hes_clme_ver, st->hes_system_number,
                     st->gateway_class, st->public_description, inv.hans_number, inv.wans_number,
                     inv.service_modules_number, st->vulnerability, st->adverse_impact,
                     st->likelihood, st->risk_level);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        log_error("id: status answer does not fit %d bytes; it is truncated", (int)sizeof(buf));
    }

    hes_msg_set_payload_str(out, buf);
}

////////////////////////////////////////////////////////////////////////////////
/// Table 39 discovery, memoryType 'op'. A functional action ('ac' address), so
/// this is invoked rather than read: it reports where the gateway's binding maps
/// are.
///
/// One row per binding map, ';' between rows and ',' between fields -- the
/// convention the A&A service's tables already use in this POC.
static void id_discovery_on_get(service_object_t* so, hes_clme_msg_t* out)
{
    id_service_state_t* st = (id_service_state_t*)so->state;
    id_inventory_t inv;
    char buf[HES_PAYLOAD_MAX];
    size_t used = 0;

    buf[0] = '\0';
    memset(&inv, 0, sizeof(inv));
    if (st->inventory) {
        st->inventory(st->inventory_ctx, &inv);
    }

    for (int i = 0; i < inv.n_binding_maps && i < ID_MAX_BINDING_MAPS; i++) {
        const id_binding_map_t* m = &inv.binding_maps[i];
        int n = snprintf(buf + used, sizeof(buf) - used,
                         "%smt=%s,mi=%" PRIu32 ",ni=%" PRIu32 ",ad=%s", (i == 0) ? "" : ";",
                         m->module_type, m->module_ref_index, m->net_ref_index, m->address);
        if (n < 0 || (size_t)n >= sizeof(buf) - used) {
            log_error("id: discovery answer truncated at binding map %d of %d", i + 1,
                      inv.n_binding_maps);
            break;
        }
        used += (size_t)n;
    }

    hes_msg_set_payload_str(out, buf);
}

////////////////////////////////////////////////////////////////////////////////
/// Answers a PUT by refusing it: every table this service holds is preset, so
/// nothing here is changeable over HES-CLME. The refusal is logged rather than
/// either silently dropped or pretended to be applied.
///
/// @param so The service object (unused).
/// @param in The PUT being refused; its path goes to the log.
static void id_on_put(service_object_t* so, const hes_clme_msg_t* in)
{
    // Every data table of this service is preset: 'pr' and 'ro' are ROM
    // (preset), 'sp' is Secret Programmed (preset), 'po' is ROM-post (every
    // entry preset by the installer). Nothing here is changeable via HES-CLME,
    // so log and ignore rather than pretend to apply a change that cannot
    // happen.
    (void)so;
    log_info("id: PUT to %s ignored (the identification service is read-only)", in->path);
}

////////////////////////////////////////////////////////////////////////////////
/// Releases the service state. One state is shared by the objects at every
/// address, so only the last object out scrubs the secret material (the
/// fingerprint and the exported key) and frees the state.
///
/// @param so The service object being destroyed.
static void id_destroy(service_object_t* so)
{
    id_service_state_t* st = (id_service_state_t*)so->state;
    if (!st) {
        return;
    }
    so->state = NULL;

    // The state is shared by the objects at every address, so only the last one
    // out scrubs the secret and frees it.
    if (--st->refs > 0) {
        return;
    }
    secure_wipe(st, sizeof(*st));
    free(st);
}

////////////////////////////////////////////////////////////////////////////////
/// One service object per Lexicon address: same service, three doors.
typedef struct id_address {
    const char* path;
    void (*on_get)(service_object_t* so, hes_clme_msg_t* out);
} id_address_t;

static const id_address_t ID_ADDRESSES[ID_SERVICE_OBJECT_COUNT] = {
        {HES_LX_ID, id_config_on_get},                 // Table 36/37: capability
        {HES_LX_ID_CENTRALOPS_CV, id_status_on_get},   // Tables 40-44: the values
        {HES_LX_ID_DISCOVER_BM, id_discovery_on_get},  // Table 38/39: the action
};

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

int id_service_create(const char* identity_path, service_object_t** out, int max_out)
{
    if (!identity_path || !*identity_path) {
        log_error("id: no identity document provisioned (pass --identity <file>)");
        return -1;
    }
    if (!out || max_out < ID_SERVICE_OBJECT_COUNT) {
        log_error("id: need room for %d service objects", ID_SERVICE_OBJECT_COUNT);
        return -1;
    }

    id_service_state_t* st = calloc(1, sizeof(*st));
    if (!st) {
        log_error("id: out of memory");
        return -1;
    }

    if (load_identity(st, identity_path) != 0) {
        secure_wipe(st, sizeof(*st));
        free(st);
        log_error("id: refusing to start without a valid identity document");
        return -1;
    }

    for (int i = 0; i < ID_SERVICE_OBJECT_COUNT; i++) {
        service_object_t* so = calloc(1, sizeof(*so));
        if (!so) {
            for (int j = 0; j < i; j++) {
                out[j]->destroy(out[j]);
                free(out[j]);
                out[j] = NULL;
            }
            secure_wipe(st, sizeof(*st));
            free(st);
            log_error("id: out of memory");
            return -1;
        }

        hes_strlcpy(so->path, sizeof(so->path), ID_ADDRESSES[i].path);
        so->state = st;
        so->on_get = ID_ADDRESSES[i].on_get;
        so->on_put = id_on_put;
        so->tick = NULL;  // identification service has no autonomous behaviour
        so->destroy = id_destroy;
        st->refs++;
        out[i] = so;
    }
    return ID_SERVICE_OBJECT_COUNT;
}

void id_service_set_inventory_source(service_object_t* so, id_inventory_fn fn, void* ctx)
{
    if (!so || !so->state) {
        return;
    }
    id_service_state_t* st = (id_service_state_t*)so->state;
    st->inventory = fn;
    st->inventory_ctx = ctx;
}

int id_service_export_identity_key(service_object_t* so, unsigned char priv[ID_IDENTITY_KEY_BYTES])
{
    if (!so || !so->state || !priv) {
        return -1;
    }
    id_service_state_t* st = (id_service_state_t*)so->state;
    if (!st->have_identity_key) {
        return -1;  // already handed over, or no key was derived
    }

    memcpy(priv, st->identity_scalar, ID_IDENTITY_KEY_BYTES);

    // Custody moves: the caller is now the single holder of the secret. Wiping
    // here rather than at destroy keeps at most one copy in the process.
    secure_wipe(st->identity_scalar, sizeof(st->identity_scalar));
    st->have_identity_key = 0;
    return 0;
}
