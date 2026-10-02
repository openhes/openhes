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
/// @brief Authorization and Authentication service (foundational, ISO/IEC
/// 18012-3 clause 11.2.4).
///
/// @details
/// Answers two questions: who is this (authentication), and what may they do
/// (authorization). The important idea is that the service does not sit in the
/// path of every message: it *writes binding-map policy* (the authorType 'at'
/// field of a binding-map output row), and the binding map enforces it locally.
///
/// The policy is a separate, read-only document (memoryType 'sp' in the
/// standard: secret, preset, and never readable over HES-CLME). See README.md
/// for the model, the JSON schema, worked examples, and why the policy lives in
/// its own file instead of the product profile.
///
/// A note on the address: the service object answers on
///   /lx/ob/so/aa/ac/cv     (service 'aa', functionalObject 'ac' authClasses)
/// and GET deliberately exposes only class id/name/status -- never credentials.

#ifndef OPENHES_SRC_SERVICES_AUTH_AUTH_H
#define OPENHES_SRC_SERVICES_AUTH_AUTH_H

#include "common/service_object.h"

////////////////////////////////////////////////////////////////////////////////
/// Callback the authorization service uses to write a permission into a binding
/// map's authorType ('at') field.
///
/// @param ctx Opaque host pointer -- the host's binding map.
/// @param at_address Binding-map address, e.g. "/lx/ob/bm/ot1/op1/at".
/// @param value 'bk' (block) | 'fl' (flow) | 'pa' (partial).
/// @return 0 on success, non-zero when no binding-map row carries that address.
typedef int (*auth_at_writer_fn)(void* ctx, const char* at_address, const char* value);

////////////////////////////////////////////////////////////////////////////////
/// Creates the service object and loads its policy. The policy is read once and
/// then immutable: the standard's memoryType 'sp' (Secret Programmed) means it is
/// preset and not changeable via HES-CLME. To change authorization, edit the
/// policy file and restart -- there is deliberately no programmable path.
///
/// @param policy_path Path to the policy document, or NULL for an empty policy.
/// @return The service object, or NULL on allocation failure.
service_object_t* auth_service_create(const char* policy_path);

////////////////////////////////////////////////////////////////////////////////
/// Initial authorization: for every class, write each applicable permission's
/// valueNew ('vn') into its target binding-map authorType field ('lo'). This is
/// the whole point of the service -- see README.md.
///
/// Call once at startup, after the binding map is loaded, and again after any
/// policy change. In this POC the binding map and this service share a process,
/// so the write is a direct call; the same address would be a PUT if the binding
/// map lived in another module.
///
/// @param so The A&A service object.
/// @param write The writer to call for each applicable permission.
/// @param ctx Opaque pointer handed back to `write` (the host's binding map).
/// @return The number of successful writes.
int auth_service_propagate(service_object_t* so, auth_at_writer_fn write, void* ctx);

////////////////////////////////////////////////////////////////////////////////
/// Mode B -- authorize ONE message (18012-3 Table 134, 'ap' = 'au').
///
/// Verifies the presented credential against the credentials table, resolves the
/// class it belongs to, and consults that class's permission rows for the given
/// binding-map authorType address. This is the per-message counterpart of
/// auth_service_propagate(): same tables, same predicate, but the answer is
/// applied to a single message instead of stored in the field.
///
/// Fails closed: an unknown or invalid credential, a credential that maps to no
/// class, or no applicable permission all return 0. The credential itself is
/// never logged, echoed, or returned.
///
/// @param so The A&A service object.
/// @param cred_info Presented credential ('ci').
/// @param user_name Claimed user name ('un'), or NULL/empty if not supplied.
/// @param at_address Binding-map authorType address, e.g. "/lx/ob/bm/ot1/op1/at".
/// @return 1 when authorized, 0 when denied.
int auth_service_authorize(service_object_t* so,
                           const char* cred_info,
                           const char* user_name,
                           const char* at_address);

////////////////////////////////////////////////////////////////////////////////
/// Does the policy demand a proof of identity for this operation, and if so,
/// whose?
///
/// 18012-3 has no table for this: permissionService (Table 68) says what a class
/// may do, never how it must prove itself. So the requirement is declared in the
/// policy document's own "proofRequired" section, which keeps the standard's
/// rows untouched. The proof itself is the crypto service's business -- this
/// only answers the policy question, and the caller then decides.
///
/// @param so The A&A service object.
/// @param at_address Binding-map authorType address, e.g. "/lx/ob/bm/ot1/op1/at".
/// @return The 64-character public ID the proof must demonstrate, or NULL when
///         no proof is required.
const char* auth_service_required_proof_pi(service_object_t* so, const char* at_address);

#endif  // #ifndef OPENHES_SRC_SERVICES_AUTH_AUTH_H
