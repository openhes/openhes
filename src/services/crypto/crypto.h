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
/// @brief Cryptographic service (foundational, ISO/IEC 18012-3 11.2.5, tables
/// 70-73): the cipher set, and the gateway identity's signature operation.
///
/// @details
/// TWO addresses, following 11.2.5.2 and 11.2.5.3:
///   /lx/ob/so/cr      Tables 70/71, memoryType 'pr' -- the CAPABILITY
///                     declaration ('vr' 'rq' 'si'), which 11.2.5.2 fixes as
///                     '1.0.0', 'ma' and 'no' (a single instance is NOT
///                     required -- unlike the identification service).
///   /lx/ob/so/cr/ci   Tables 72/73, memoryType 'sp' -- the CIPHERS.
/// Each row is indexed by encryptIndex ('ei') -- exactly the query
/// parameter used in the standard's own worked example:
///   get /lx/ob/uo/hv/ts/as/cv/va?ei=1
/// i.e. "ei=1" selects which cipher configuration to apply. CBC and CFB
/// payloads are uppercase/lowercase hex ciphertext with a zero IV. CCM and
/// GCM payloads are hex(nonce || ciphertext || authentication-tag), with a
/// 12-byte nonce, a 16-byte tag, and no additional authenticated data.
///
/// The implementation uses Mbed TLS PSA Crypto for AES and ARIA. Blowfish is
/// advertised as unsupported because it is not available in this Mbed TLS
/// build. Unsupported algorithms, modes, combinations, invalid parameters,
/// and failed AEAD authentication are returned as service-level errors.
///
/// ---------------------------------------------------------------------------
/// The identity-signature operation (the identity key pair)
///
/// 11.2.2.1 says the identification service's digitalFingerprint is "used for
/// encryption techniques", and that the public ID exists "so that masquerade
/// gateways cannot pretend to be legitimate gateways". The gateway therefore
/// holds a key pair derived from 'fp' (see services/id/id.h) and can SIGN a
/// challenge, which anyone holding the gateway's public 'pi' can verify.
///
/// This is the one component allowed to touch that key: the identification
/// service wipes its copy as soon as this service takes custody.
///
/// The operation is NOT a cipher, and 18012-3 has no transCode for a signature,
/// so it is described by the two Table 73 fields that exist for a non-cipher
/// operation: 'it' (internal operations, in bits) and 'au' (authorization size,
/// in bytes). Its row on /lx/ob/so/cr/ci reports `ei`, `ks`, `it` and `au`, and
/// introduces no new address and no new enumerated value.
///
/// This is a documented extension: Table 73's 'nc' enumerates ciphers only.
/// See docs/poc_design.md.

#ifndef OPENHES_SRC_SERVICES_CRYPTO_CRYPTO_H
#define OPENHES_SRC_SERVICES_CRYPTO_CRYPTO_H

#include <stddef.h>

#include "common/service_object.h"

/// The encryptIndex of the internal identity-signature operation (Table 73's
///  'it'/'au' row). Chosen above the cipher rows so the two never collide.
#define CRYPTO_IDENTITY_EI 100

/// Bytes in the identity key: a P-256 scalar.
#define CRYPTO_IDENTITY_KEY_BYTES 32

/// Bytes in a P-256 signature, as PSA emits it: r || s.
#define CRYPTO_IDENTITY_SIG_BYTES 64

/// Bytes in an uncompressed P-256 public point: 0x04 || X || Y.
#define CRYPTO_IDENTITY_PUB_BYTES 65

/// One service object per Lexicon address: same service, two doors.
#define CRYPTO_SERVICE_OBJECT_COUNT 2
#define CRYPTO_OBJECT_CONFIG 0   ///< /lx/ob/so/cr    -- Tables 70/71
#define CRYPTO_OBJECT_CIPHERS 1  ///< /lx/ob/so/cr/ci -- Tables 72/73

////////////////////////////////////////////////////////////////////////////////
/// Create the service's objects, one per address, all sharing one state.
///
/// @param out     Receives CRYPTO_SERVICE_OBJECT_COUNT objects.
/// @param max_out Room in 'out'.
/// @return the number of objects created, or -1 (creating nothing).
int crypto_service_create(service_object_t** out, int max_out);

////////////////////////////////////////////////////////////////////////////////
/// Take custody of the identity key derived by the identification service.
///
/// Called once, at startup, by the core module -- which gets the key from
/// id_service_export_identity_key(), the call that also wipes the identification
/// service's own copy. The scalar is wiped again when this service is destroyed.
void crypto_service_set_identity_key(service_object_t* so,
                                     const unsigned char priv[CRYPTO_IDENTITY_KEY_BYTES]);

////////////////////////////////////////////////////////////////////////////////
/// Sign a message with the gateway's identity key: SHA-256, then DETERMINISTIC
/// ECDSA over P-256.
///
/// Deterministic (RFC 6979) rather than randomised on purpose: ordinary ECDSA
/// leaks the private key if the per-signature nonce ever repeats, and this
/// removes the RNG from that path entirely. A useful side effect is that a given
/// key and message always give the same signature, so this is testable without
/// any randomness.
///
/// @param so      An object from crypto_service_create().
/// @param msg     The challenge / message to sign.
/// @param sig     Receives CRYPTO_IDENTITY_SIG_BYTES.
/// @return 0 on success, -1 when no identity key is installed or signing failed.
int crypto_service_sign(service_object_t* so,
                        const unsigned char* msg,
                        size_t msg_len,
                        unsigned char sig[CRYPTO_IDENTITY_SIG_BYTES]);

////////////////////////////////////////////////////////////////////////////////
/// Export the gateway's OWN public key, for presenting alongside a signature.
///
/// This is the counterpart to crypto_service_sign(): a verifier needs the key as
/// well as the signature, and pins it by checking its X coordinate against the
/// 'pi' the gateway publishes. It is not a secret -- 'pi' is derived from it and
/// served on HES-CLME -- so handing it out is safe.
///
/// @param pub Receives CRYPTO_IDENTITY_PUB_BYTES (0x04 || X || Y).
/// @return 0 on success, -1 when no identity key is installed.
int crypto_service_public_key(service_object_t* so, unsigned char pub[CRYPTO_IDENTITY_PUB_BYTES]);

////////////////////////////////////////////////////////////////////////////////
/// Verify a message against a PRESENTED public key.
///
/// Note what is and is not pinned here: this checks that the signature matches
/// the public key it is handed. Deciding that the key belongs to the expected
/// peer is the caller's job, and is done by comparing the key's X coordinate
/// with the expected 'pi' (crypto_pi_matches() does that comparison).
///
/// The public key is passed whole because rebuilding a point from its X alone
/// needs field arithmetic in the verifier; verifying against a presented key and
/// pinning its X to 'pi' gets the same assurance with no hand-rolled curve
/// maths. See docs/poc_design.md.
///
/// @return 0 when the signature is good, -1 otherwise.
int crypto_verify_message(const unsigned char pub[CRYPTO_IDENTITY_PUB_BYTES],
                          const unsigned char* msg,
                          size_t msg_len,
                          const unsigned char sig[CRYPTO_IDENTITY_SIG_BYTES]);

////////////////////////////////////////////////////////////////////////////////
/// Does 'pub' belong to the identity named by 'pi' (64 lowercase hex characters)?
///
/// This is the pinning step: signature verification proves possession of a key,
/// and this proves the key is the one the peer claims to be.
///
/// @return 1 when they match, 0 otherwise.
int crypto_pi_matches(const unsigned char pub[CRYPTO_IDENTITY_PUB_BYTES], const char* pi);

////////////////////////////////////////////////////////////////////////////////
/// Frame the bytes a proof covers: the message's Lexicon path, then a newline,
/// then its payload.
///
/// The path is part of the signed material so that a proof cannot be lifted onto
/// a different message -- signing the payload alone would let anyone replay the
/// same signature at another address. A newline separates them because a Lexicon
/// path never contains one, so the framing is unambiguous.
///
/// Both the signer and the verifier must frame identically, which is why this is
/// exported rather than inlined at each call site.
///
/// @param out     Receives the framed bytes, NUL-terminated.
/// @param out_cap Room in 'out'; needs HES_PATH_MAX + HES_PAYLOAD_MAX + 2.
/// @return the number of bytes to sign (excluding the terminator).
size_t crypto_proof_message(const char* path, const char* payload, char* out, size_t out_cap);

////////////////////////////////////////////////////////////////////////////////
/// Verify a PROOF presented in a message's 'ci' (credInfo) field.
///
/// The proof is the same three records the crypto service itself answers a
/// challenge with, so a peer proves itself exactly the way this gateway proves
/// itself:
///   pi=<64 hex>    the identity being claimed
///   pk=<130 hex>   the public key that identity names
///   sig=<128 hex>  a signature over crypto_proof_message()'s bytes
///
/// Both halves are checked, and both matter: 'sig' against 'pk' proves the
/// sender holds the private key, and 'pk' pinned to 'expected_pi' proves it is
/// the identity the policy expects. Skipping the second would accept a valid
/// signature from anybody, which is no authorization at all.
///
/// The caller decides WHEN a proof is required (the A&A policy does); this only
/// answers whether the one presented is good.
///
/// @param cred_info   The message's 'ci' field: fields after the credential, if
///                    any, are searched for pi/pk/sig.
/// @param expected_pi The public ID the policy requires (64 hex characters).
/// @param msg         The framed bytes from crypto_proof_message().
/// @return 1 when the proof is valid and belongs to expected_pi, 0 otherwise.
int crypto_verify_proof(const char* cred_info,
                        const char* expected_pi,
                        const unsigned char* msg,
                        size_t msg_len);

#endif  // #ifndef OPENHES_SRC_SERVICES_CRYPTO_CRYPTO_H
