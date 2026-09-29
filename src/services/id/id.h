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
/// @brief Identification service (foundational, ISO/IEC 18012-3 11.2.2): the
/// gateway's public ID ('pi') and its secret fingerprint ('fp').
///
/// @details
/// "Every HES gateway shall have one (and only one) Identification
/// Service. [It provides] a unique public ID that is publicly
/// accessible, yet anonymous ... and a secret internal random code
/// (digital_fingerprint) that is not revealed."
///
/// ---------------------------------------------------------------------------
/// pi and fp are a key pair
///
/// The standard's own words are that the public ID exists "so that masquerade
/// gateways cannot pretend to be legitimate gateways", and that the fingerprint
/// is "a secret internal random code ... used for encryption techniques". The
/// shape that satisfies both is a KEY PAIR:
///
///   'fp'  the secret root. Never served (memoryType 'sp'), never logged, never
///         copied out of this service, and wiped when the last object dies.
///   'pi'  the PUBLIC key derived from it. This is what makes the pair useful:
///         a verifier holding only 'pi' can check a signature the gateway made
///         with 'fp', and 'pi' stays "publicly accessible, yet anonymous" -- a
///         public key is a pseudonym, and reveals neither location nor date.
///
/// The derivation is deterministic (HKDF-SHA256 -> a P-256 scalar), so 'pi' is
/// STATIONARY for as long as 'fp' is: no extra storage, nothing to back up, and
/// the same identity across restarts.
///
/// 'pi' is 64 hex characters because it is a 256-bit field, and a P-256 point
/// does not fit in that: 'pi' is therefore the point's X coordinate, with Y
/// required to be the EVEN root. That convention is what makes "pi = X" a
/// complete description of the public key, and the derivation enforces it (if
/// the first candidate has an odd Y, its negated scalar -- the same X, Y
/// negated -- is used instead).
///
/// Deviation to remember: 11.2.2.1 describes BOTH values as 256-bit TRUE RANDOM
/// numbers, so a derived 'pi' is not random. The clause's stated requirement is
/// still met ("information relating to the gateway cannot be derived from the
/// ID" -- a public key leaks no location and no generation date), and _fp_ itself
/// remains a true random number. This is a documented POC decision, not an
/// oversight; see docs/poc_design.md.
///
/// ---------------------------------------------------------------------------
/// The service answers on three Lexicon addresses. That is why 11.2.2 looks like
/// so many tables: the ADDRESS says *where*, the memoryType says *who may touch
/// it* -- and three of the four data tables share one address.
///
///   /lx/ob/so/id            Table 37 configurationData ('pr')
///                           rq requirement 'ma', si single 'ye'
///
///   /lx/ob/so/id/co/st/cv   Table 41 Data             ('ro') pi vr hs pd ch
///                           Table 42 operationData    ('op') nh nw ns
///                           Table 43 Data             ('sp') fp   <-- SECRET
///                           Table 44 metaData         ('po') vl il ll rl
///
///   /lx/ac/so/id/dc/bm/cv   Table 39 discovery        ('op')
///                           the binding maps present in this gateway
///
/// Note 'ac' (functional action) rather than 'ob' (object) for discovery: it is
/// something you invoke, not a value you read (Clause 7.1).
///
/// Reach one datum by appending its transCode to the address: the skeleton here
/// is /lx/ob/so/id and 'rq' is data at it, so `get /lx/ob/so/id/rq` -> "ma"
/// (18012-4 5.2.8.2.2 does the same with '/lx/ob/uo/li/ll/da/cv/va'). Several
/// items can be fetched at once with the 'da' query: `?da=rq,si`. Note that
/// configurationData and metaData are NOT writable by a client at all
/// (18012-4 5.2.9.3.4), so these are read-only by the standard, not just by
/// choice. See README.md, "How you actually get and set one datum".
///
/// 'fp' is never served: Table 43 gives it 'sp' (Secret Programmed), i.e.
/// obscured -- not readable over HES-CLME "without proper keys".
///
/// The provisioned values come from an identity document loaded by
/// id_service_create(). In a product, 'pi' would live in read-only storage and
/// 'fp' would be factory-written to a secure element; the document stands in for
/// both. See README.md.

#ifndef OPENHES_SRC_SERVICES_ID_ID_H
#define OPENHES_SRC_SERVICES_ID_ID_H

#include "common/service_object.h"

/// How many service objects id_service_create() produces (one per address).
#define ID_SERVICE_OBJECT_COUNT 3

/// Room for the discovered binding maps handed back by the inventory hook.
#define ID_MAX_BINDING_MAPS 8

////////////////////////////////////////////////////////////////////////////////
/// One discovered binding map (18012-3 Table 39).
///
/// @note The standard's Object address table calls this "discovery ... of
/// bindingMaps", while its data table describes modules; this POC therefore
/// reports the module that HOSTS each binding map.
typedef struct id_binding_map {
    char module_type[4];        ///< 'mt': hi (HAN iface) | wi (WAN iface) | sm (service module)
    uint32_t module_ref_index;  ///< 'mi': 0 = not applicable / not initialized
    uint32_t net_ref_index;     ///< 'ni': 0 = the module itself
    char address[64];  ///< 'ad': an interface module's IP address; empty for a service module
} id_binding_map_t;

////////////////////////////////////////////////////////////////////////////////
/// What the gateway looks like from the identification service's point of view.
///
/// The id service cannot learn these by itself -- the knowledge belongs to the
/// host. The host answers from the gateway's module roster (the Product Profile
/// Manifest's modules[], held by the Device Manifest service), and falls back to
/// counting the binding map's addressing table when no manifest service is
/// configured. A modular gateway would refresh this from a discovery probe over
/// HES-CLME instead; see README.md, "Where the inventory comes from".
///
/// @note 'ns' and the binding-map rows describe the same thing: a service module
/// is a module that contains a binding map (15045-4-1 A.4), and 18012-3 glosses
/// 'ns' itself as "number of binding maps". They are separate fields because
/// they feed separate answers -- the counts into Table 42's 'nh nw ns', the rows
/// into Table 39's 'mt,mi,ni,ad' list -- and because they may legitimately
/// diverge: the count stays exact when binding_maps[] is full, and the
/// addressing-table fallback can count a service module it emits no row for.
typedef struct id_inventory {
    int hans_number;             ///< 'nh': HAN interface modules present (Table 42)
    int wans_number;             ///< 'nw': WAN interface modules present
    int service_modules_number;  ///< 'ns': service modules present (see the note above)
    int n_binding_maps;          ///< how many entries binding_maps[] holds
    id_binding_map_t binding_maps[ID_MAX_BINDING_MAPS];
} id_inventory_t;

////////////////////////////////////////////////////////////////////////////////
/// Answers the gateway-inventory question above (see id_service_set_inventory_source).
typedef void (*id_inventory_fn)(void* ctx, id_inventory_t* out);

////////////////////////////////////////////////////////////////////////////////
/// Build every identification-service object -- one per Lexicon address.
///
/// @param identity_path Path to the identity document (JSON), holding the
///        gateway's 256-bit 'pi' and 'fp' as 64 hex characters each, plus the
///        optional 'vr'/'hs'/'pd'/'ch' and the Table 44 metaData 'vl'/'il'/
///        'll'/'rl'. Required: passing NULL or an empty path, a missing file, a
///        malformed document, or values that are not exactly 64 hex characters
///        all fail closed and create nothing, because a gateway without a
///        well-formed, stationary identity must not start.
/// @param out Receives the created objects, in address order.
/// @param max_out Capacity of out; at least ID_SERVICE_OBJECT_COUNT.
/// @return The number of objects written to out, or -1 on error (nothing is
///         written to out). Caller owns each object: so->destroy(so), then
///         free(so).
int id_service_create(const char* identity_path, service_object_t** out, int max_out);

////////////////////////////////////////////////////////////////////////////////
/// Registers where the gateway-inventory questions are answered (Tables 39/42).
///
/// Optional: without it, the counts and the discovery list come back as zero and
/// empty, which is honest -- the service genuinely does not know.
///
/// @param so Any object returned by id_service_create(): they share one state,
///        so registering on any of them is equivalent.
/// @param fn The callback; NULL clears it.
/// @param ctx Opaque pointer handed back to fn (for the core module, the
///        binding map).
void id_service_set_inventory_source(service_object_t* so, id_inventory_fn fn, void* ctx);

/// Bytes in the identity key: a P-256 scalar.
#define ID_IDENTITY_KEY_BYTES 32

////////////////////////////////////////////////////////////////////////////////
/// Hand the identity key derived from 'fp' to the one component allowed to use
/// it, and give up this service's own copy.
///
/// 11.2.2.1: the fingerprint is "used for encryption techniques" -- so the
/// component that performs cryptography needs the key, and no one else does.
/// Calling this copies the private scalar out and then WIPES it here, so at most
/// one copy of the secret exists in the process, in the hands of the component
/// that legitimately uses it. 'fp' itself is never copied out at all.
///
/// It is a one-shot handover: a second call fails, because there is nothing left
/// to give. The core module calls it once, right after creating the crypto
/// service.
///
/// @param so   Any object returned by id_service_create().
/// @param priv Receives the 32-byte private scalar.
/// @return 0 on success, -1 when there is no key (already handed over, or the
///         service was created without one).
int id_service_export_identity_key(service_object_t* so, unsigned char priv[ID_IDENTITY_KEY_BYTES]);

#endif  // #ifndef OPENHES_SRC_SERVICES_ID_ID_H
