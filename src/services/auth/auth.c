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
/// @brief Implementation of the A&A service (auth.h): the policy document, the
/// runtime tables, and the hooks the binding map calls.
///
/// @details
/// Three parts:
///
///   - the policy loader: parses the read-only JSON policy into the file-static
///     tables (authClasses, authIdentity, credentials, permissionGroups,
///     permissions, proofRequired), bounded by the AUTH_MAX_* limits.
///
///   - propagation: writes each row's authorType ('at') into the binding map, so
///     enforcement happens locally in the map rather than in the message path --
///     see auth.h for why the service is not a proxy.
///
///   - the decision hook: the per-message authorization callback the binding map
///     calls for a row whose output requires proof (Mode B: the credential on the
///     incoming message), plus the identity/credential lookups behind it.

#include "auth.h"

#include <jansson.h>
#include <log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUTH_MAX_CLASSES 16
#define AUTH_MAX_IDENTITIES 16
#define AUTH_MAX_CREDENTIALS 16
#define AUTH_MAX_GROUPS 32
#define AUTH_MAX_PERMISSIONS 32
#define AUTH_MAX_PROOF_REQUIREMENTS 16
#define AUTH_NAME_MAX 32
#define AUTH_SECRET_MAX 64

////////////////////////////////////////////////////////////////////////////////
/// One row of authClasses ('ac', 18012-3 Table 60).
typedef struct auth_class {
    int class_id;              ///< 'ci'
    char name[AUTH_NAME_MAX];  ///< 'fn'
    char status[4];            ///< 'st': ni | na | au
    int identity_ref;          ///< 'ti' -> authIdentity
    int group_id;              ///< 'pi' -> permissionGroup
    int credential_ref;        ///< 'cr' -> credentials
} auth_class_t;

////////////////////////////////////////////////////////////////////////////////
/// One row of authIdentity ('ai', Table 62): what an identity attaches to.
/// 'ta' = 'dv' means "this identity is that device".
typedef struct auth_identity {
    int identity;           ///< 'id'
    char type[4];           ///< 'ta': an (name) | dv (device)
    uint32_t device_index;  ///< 'di'
} auth_identity_t;

////////////////////////////////////////////////////////////////////////////////
/// One row of credentials ('cr', Table 64).
///
/// SECRET: 'info' holds a password / key / token. It must never be logged,
/// echoed in a reply, or returned from a GET -- the standard marks this table
/// memoryType 'sp' (obscured).
typedef struct auth_credential {
    int cred_id;                 ///< 'id'
    char method[4];              ///< 'cm': ak (key) | pw (password)
    char protocol[4];            ///< 'cp': oa | tl
    char info[AUTH_SECRET_MAX];  ///< 'ci': the secret itself
    long timing;                 ///< 'ss': secondsSinceEpoch
    int valid;                   ///< 'cv': 1 = 'ye'
} auth_credential_t;

////////////////////////////////////////////////////////////////////////////////
/// One row of permissionGroup ('pg', Table 66). Several rows may share the same
/// 'pi' -- that is how one group carries more than one permission (the standard
/// allows it because permissionGroupID "does not require uniqueness").
typedef struct auth_group {
    int group_id;        ///< 'pi'
    char table_type[4];  ///< 'pt': ps
    int permission_ref;  ///< 'pr' -> auth_permission_t.service_index
} auth_group_t;

////////////////////////////////////////////////////////////////////////////////
/// One row of permissionService ('ps', Table 68) -- the actual instruction:
/// "when statusCheck matches the class status, write valueNew into the
/// binding-map field named by lexicon_object".
typedef struct auth_permission {
    int service_index;                  ///< 'pi'
    char status_check[4];               ///< 'st': ni | na | au
    uint32_t device_index;              ///< 'di'
    char lexicon_object[HES_PATH_MAX];  ///< 'lo': e.g. /lx/ob/bm/ot1/op1/at
    char value_new[4];                  ///< 'vn': bk | fl | pa
} auth_permission_t;

////////////////////////////////////////////////////////////////////////////////
/// One entry of the policy's "proofRequired" section -- a POC declaration, not a
/// table of 18012-3.
///
/// 18012-3 has nowhere to say "this operation must be accompanied by a proof of
/// the sender's identity": Table 68's permissionService says what a class may
/// do, never how it must prove itself. So the requirement lives in this
/// document's own section rather than as an extra key inside a standard row.
///
/// Absence means no proof is needed; presence means a proof is MANDATORY, so
/// forgetting to send one is a denial rather than a silent pass.
typedef struct auth_proof_requirement {
    char at_address[HES_PATH_MAX];  ///< the authorType field this governs
    char public_id[65];             ///< the 'pi' the proof must demonstrate
} auth_proof_requirement_t;

typedef struct auth_service_state {
    auth_class_t classes[AUTH_MAX_CLASSES];
    int n_classes;
    auth_identity_t identities[AUTH_MAX_IDENTITIES];
    int n_identities;
    auth_credential_t credentials[AUTH_MAX_CREDENTIALS];
    int n_credentials;
    auth_group_t groups[AUTH_MAX_GROUPS];
    int n_groups;
    auth_permission_t permissions[AUTH_MAX_PERMISSIONS];
    int n_permissions;
    auth_proof_requirement_t proof_requirements[AUTH_MAX_PROOF_REQUIREMENTS];
    int n_proof_requirements;
    int loaded;  ///< a policy document was read successfully
} auth_service_state_t;

////////////////////////////////////////////////////////////////////////////////
// Policy loading
////////////////////////////////////////////////////////////////////////////////

static int jobj_int(const json_t* o, const char* key, int def)
{
    const json_t* v = json_object_get(o, key);
    return json_is_integer(v) ? (int)json_integer_value(v) : def;
}

////////////////////////////////////////////////////////////////////////////////
/// Copies a string member of a JSON object into a fixed buffer.
///
/// A member that is missing, or is not a string, leaves `dst` as it was, so the
/// caller's initial value (normally "") stands.
///
/// @param o   The JSON object to read from.
/// @param key The member name, e.g. "fn".
/// @param dst Buffer that receives the value.
/// @param cap Capacity of dst.
static void jobj_str(const json_t* o, const char* key, char* dst, size_t cap)
{
    const json_t* v = json_object_get(o, key);
    if (json_is_string(v)) {
        hes_strlcpy(dst, cap, json_string_value(v));
    }
}

////////////////////////////////////////////////////////////////////////////////
/// Fetches an array member of a JSON object.
///
/// @param root The JSON object to read from.
/// @param key  The member name, e.g. "authClasses".
/// @return The array, or NULL when the member is missing or is not an array.
static const json_t* jobj_array(const json_t* root, const char* key)
{
    const json_t* v = json_object_get(root, key);
    return json_is_array(v) ? v : NULL;
}

////////////////////////////////////////////////////////////////////////////////
/// Loads the (secret, read-only) policy document.
///
/// Missing arrays are simply empty, and a malformed file leaves an empty policy
/// with an error logged, so the gateway still starts. The contents are never
/// logged: the credentials table holds secrets.
///
/// @param st The service state to fill.
/// @param path Path to the policy document; NULL or "" for an empty policy.
static void load_policy(auth_service_state_t* st, const char* path)
{
    // "" counts as "no policy": an omitted --auth-policy reaches us as an empty
    // string rather than NULL (argtable initializes string arguments to ""),
    // and opening the file "" would report a policy that was never given as one
    // that could not be read.
    if (path == NULL || path[0] == '\0') {
        log_info("auth: no policy file given (empty authorization tables)");
        return;
    }

    json_error_t err;
    json_t* root = json_load_file(path, 0, &err);
    if (!root) {
        log_error("auth: cannot read policy '%s': %s", path, err.text);
        return;
    }

    const json_t* arr = jobj_array(root, "authClasses");
    if (arr) {
        size_t n = json_array_size(arr);
        for (size_t i = 0; i < n && st->n_classes < AUTH_MAX_CLASSES; i++) {
            const json_t* row = json_array_get(arr, i);
            if (!json_is_object(row)) {
                continue;
            }
            auth_class_t* c = &st->classes[st->n_classes];
            c->class_id = jobj_int(row, "ci", 0);
            jobj_str(row, "fn", c->name, sizeof(c->name));
            jobj_str(row, "st", c->status, sizeof(c->status));
            c->identity_ref = jobj_int(row, "ti", 0);
            c->group_id = jobj_int(row, "pi", 0);
            c->credential_ref = jobj_int(row, "cr", 0);
            st->n_classes++;
        }
    }

    arr = jobj_array(root, "authIdentities");
    if (arr) {
        size_t n = json_array_size(arr);
        for (size_t i = 0; i < n && st->n_identities < AUTH_MAX_IDENTITIES; i++) {
            const json_t* row = json_array_get(arr, i);
            if (!json_is_object(row)) {
                continue;
            }
            auth_identity_t* id = &st->identities[st->n_identities];
            id->identity = jobj_int(row, "id", 0);
            jobj_str(row, "ta", id->type, sizeof(id->type));
            id->device_index = (uint32_t)jobj_int(row, "di", 0);
            st->n_identities++;
        }
    }

    arr = jobj_array(root, "credentials");
    if (arr) {
        size_t n = json_array_size(arr);
        for (size_t i = 0; i < n && st->n_credentials < AUTH_MAX_CREDENTIALS; i++) {
            const json_t* row = json_array_get(arr, i);
            if (!json_is_object(row)) {
                continue;
            }
            auth_credential_t* cr = &st->credentials[st->n_credentials];
            char valid[4] = {0};
            cr->cred_id = jobj_int(row, "id", 0);
            jobj_str(row, "cm", cr->method, sizeof(cr->method));
            jobj_str(row, "cp", cr->protocol, sizeof(cr->protocol));
            jobj_str(row, "ci", cr->info, sizeof(cr->info));
            cr->timing = (long)jobj_int(row, "ss", 0);
            jobj_str(row, "cv", valid, sizeof(valid));
            cr->valid = (strcmp(valid, "ye") == 0);
            st->n_credentials++;
        }
    }

    arr = jobj_array(root, "permissionGroups");
    if (arr) {
        size_t n = json_array_size(arr);
        for (size_t i = 0; i < n && st->n_groups < AUTH_MAX_GROUPS; i++) {
            const json_t* row = json_array_get(arr, i);
            if (!json_is_object(row)) {
                continue;
            }
            auth_group_t* g = &st->groups[st->n_groups];
            g->group_id = jobj_int(row, "pi", 0);
            jobj_str(row, "pt", g->table_type, sizeof(g->table_type));
            g->permission_ref = jobj_int(row, "pr", 0);
            st->n_groups++;
        }
    }

    arr = jobj_array(root, "permissionServices");
    if (arr) {
        size_t n = json_array_size(arr);
        for (size_t i = 0; i < n && st->n_permissions < AUTH_MAX_PERMISSIONS; i++) {
            const json_t* row = json_array_get(arr, i);
            if (!json_is_object(row)) {
                continue;
            }
            auth_permission_t* p = &st->permissions[st->n_permissions];
            p->service_index = jobj_int(row, "pi", 0);
            jobj_str(row, "st", p->status_check, sizeof(p->status_check));
            p->device_index = (uint32_t)jobj_int(row, "di", 0);
            jobj_str(row, "lo", p->lexicon_object, sizeof(p->lexicon_object));
            jobj_str(row, "vn", p->value_new, sizeof(p->value_new));
            st->n_permissions++;
        }
    }

    arr = jobj_array(root, "proofRequired");
    if (arr) {
        size_t n = json_array_size(arr);
        for (size_t i = 0; i < n && st->n_proof_requirements < AUTH_MAX_PROOF_REQUIREMENTS; i++) {
            const json_t* row = json_array_get(arr, i);
            if (!json_is_object(row)) {
                continue;
            }
            auth_proof_requirement_t* pr = &st->proof_requirements[st->n_proof_requirements];
            jobj_str(row, "at", pr->at_address, sizeof(pr->at_address));
            jobj_str(row, "pi", pr->public_id, sizeof(pr->public_id));
            if (pr->at_address[0] == '\0' || strlen(pr->public_id) != 64) {
                log_error(
                        "auth: ignoring a proofRequired entry (needs an 'at' address and a "
                        "64-character 'pi')");
                continue;
            }
            st->n_proof_requirements++;
        }
    }

    json_decref(root);
    st->loaded = 1;

    log_info(
            "auth: policy loaded: %d class(es), %d identity(ies), %d credential(s), %d group "
            "row(s), %d permission row(s), %d proof requirement(s)",
            st->n_classes, st->n_identities, st->n_credentials, st->n_groups, st->n_permissions,
            st->n_proof_requirements);
}

////////////////////////////////////////////////////////////////////////////////
/// Answers a GET with the class list only: id, name, status.
///
/// Deliberately nothing else. The credentials table is memoryType 'sp'
/// (obscured) in the standard, so passwords and keys must never appear in a
/// reply -- and nothing about a credential is logged either.
///
/// @param so The service object.
/// @param out Carries the reply payload back to the caller.
static void auth_on_get(service_object_t* so, hes_clme_msg_t* out)
{
    auth_service_state_t* st = (auth_service_state_t*)so->state;
    char buf[HES_PAYLOAD_MAX];
    int off = 0;

    for (int i = 0; i < st->n_classes && off < (int)sizeof(buf) - 1; i++) {
        int n = snprintf(buf + off, sizeof(buf) - (size_t)off, "%sci=%d,fn=%s,st=%s",
                         (i > 0) ? ";" : "", st->classes[i].class_id, st->classes[i].name,
                         st->classes[i].status);
        if (n <= 0) {
            break;
        }
        off += n;
    }

    if (off == 0) {
        snprintf(buf, sizeof(buf), "no-auth-policy");
    }

    hes_msg_set_payload_str(out, buf);
}

////////////////////////////////////////////////////////////////////////////////
/// Policy writes are refused.
///
/// The authorization tables are memoryType 'sp' (Secret Programmed) in the
/// standard: preset, and not changeable via HES-CLME. There is deliberately no
/// programmable path to change authorization -- an operator edits the policy
/// file and restarts the gateway.
static void auth_on_put(service_object_t* so, const hes_clme_msg_t* in)
{
    (void)so;
    (void)in;
    log_info("auth: policy is read-only via HES-CLME ('sp' preset); PUT ignored");
}

////////////////////////////////////////////////////////////////////////////////
/// Releases the service state, i.e. the parsed policy tables. There is nothing
/// else to free: the tables live inside the state struct itself.
///
/// @param so The service object whose state is freed.
static void auth_destroy(service_object_t* so)
{
    free(so->state);
}

////////////////////////////////////////////////////////////////////////////////
/// Does the presented credential match the stored one?
///
/// The credential is everything up to the first ';'. 18012-4 5.2.5.5 calls 'ci'
/// "credential information", so this POC reads it as "the credential first, then
/// whatever accompanies it" -- which is how a proof of the sender's identity
/// rides along with a token credential without either one hiding the other. A
/// bare token has no ';' and compares exactly as it always did.
///
/// @param stored The credential from the policy (a secret: never logged).
/// @param presented The credential part of the incoming message's 'ci'.
/// @return 1 on a match.
static int credential_matches(const char* stored, const char* presented)
{
    size_t n = strcspn(presented, ";");
    return strlen(stored) == n && strncmp(stored, presented, n) == 0;
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

int auth_service_propagate(service_object_t* so, auth_at_writer_fn write, void* ctx)
{
    auth_service_state_t* st = (auth_service_state_t*)so->state;
    int writes = 0;

    // class -> (its) permissionGroup rows -> permissionService rows -> write.
    //
    // A permission row applies only when its statusCheck is exactly the class's
    // current status, per Table 68: "Check of authorization status to determine
    // data output". In this POC only one class is normally 'au' at a time (the
    // active user); other classes write nothing, so the binding map's own
    // deny-by-default 'at' value stands. That avoids two classes fighting over
    // the same field.
    for (int i = 0; i < st->n_classes; i++) {
        const auth_class_t* cls = &st->classes[i];

        for (int g = 0; g < st->n_groups; g++) {
            const auth_group_t* grp = &st->groups[g];
            if (grp->group_id != cls->group_id || strcmp(grp->table_type, "ps") != 0) {
                continue;
            }

            for (int p = 0; p < st->n_permissions; p++) {
                const auth_permission_t* perm = &st->permissions[p];
                if (perm->service_index != grp->permission_ref) {
                    continue;
                }
                if (strcmp(perm->status_check, cls->status) != 0) {
                    continue;
                }

                if (write(ctx, perm->lexicon_object, perm->value_new) == 0) {
                    log_info("auth: class '%s' (%s) -> %s = %s", cls->name, cls->status,
                             perm->lexicon_object, perm->value_new);
                    writes++;
                } else {
                    log_error("auth: class '%s': no binding-map row at '%s'", cls->name,
                              perm->lexicon_object);
                }
            }
        }
    }

    log_info("auth: propagated %d permission write(s)", writes);
    return writes;
}

int auth_service_authorize(service_object_t* so,
                           const char* cred_info,
                           const char* user_name,
                           const char* at_address)
{
    auth_service_state_t* st = (auth_service_state_t*)so->state;
    if (!st || !st->loaded || !cred_info || !cred_info[0] || !at_address) {
        return 0;  // fail closed
    }

    // 1. Find the presented credential. Note: cred_info is a secret -- never
    // log it, here or anywhere else.
    const auth_credential_t* cred = NULL;
    for (int i = 0; i < st->n_credentials; i++) {
        if (credential_matches(st->credentials[i].info, cred_info)) {
            cred = &st->credentials[i];
            break;
        }
    }
    if (!cred) {
        log_info("auth: authorization denied (unknown credential)");
        return 0;
    }
    if (!cred->valid) {
        log_info("auth: authorization denied (credential not valid)");
        return 0;
    }

    // 2. Resolve the class: the credential's identity ('id') is what the
    // classes reference through their 'cr' field.
    const auth_class_t* cls = NULL;
    for (int i = 0; i < st->n_classes; i++) {
        if (st->classes[i].credential_ref == cred->cred_id) {
            cls = &st->classes[i];
            break;
        }
    }
    if (!cls) {
        log_info("auth: authorization denied (credential maps to no class)");
        return 0;
    }

    // POC interpretation: a supplied user name must match the class's
    // user-friendly name ('fn'). Table 62's authIdentity carries a type ('an' /
    // 'dv') but no name field of its own, so 'fn' is the only name available to
    // compare 'un' against.
    if (user_name && user_name[0] && strcmp(user_name, cls->name) != 0) {
        log_info("auth: authorization denied (user name does not match class '%s')", cls->name);
        return 0;
    }

    // 3. Find the permission governing this binding-map field for the class's
    // current status -- the same walk propagation uses, applied to one message.
    const char* vn = NULL;
    for (int g = 0; g < st->n_groups; g++) {
        const auth_group_t* grp = &st->groups[g];
        if (grp->group_id != cls->group_id || strcmp(grp->table_type, "ps") != 0) {
            continue;
        }
        for (int p = 0; p < st->n_permissions; p++) {
            const auth_permission_t* perm = &st->permissions[p];
            if (perm->service_index != grp->permission_ref) {
                continue;
            }
            if (strcmp(perm->status_check, cls->status) != 0) {
                continue;
            }
            if (strcmp(perm->lexicon_object, at_address) != 0) {
                continue;
            }
            vn = perm->value_new;  // last match wins, as in propagation
        }
    }

    if (!vn) {
        log_info("auth: class '%s' (%s) has no permission for %s -- denied", cls->name, cls->status,
                 at_address);
        return 0;
    }
    if (strcmp(vn, "fl") != 0) {
        log_info("auth: class '%s' (%s) -> %s for %s -- denied", cls->name, cls->status, vn,
                 at_address);
        return 0;
    }

    log_info("auth: class '%s' (%s) -> flow for %s -- allowed", cls->name, cls->status, at_address);
    return 1;
}

const char* auth_service_required_proof_pi(service_object_t* so, const char* at_address)
{
    auth_service_state_t* st = (auth_service_state_t*)so->state;
    if (!st || !at_address) {
        return NULL;
    }

    for (int i = 0; i < st->n_proof_requirements; i++) {
        if (strcmp(st->proof_requirements[i].at_address, at_address) == 0) {
            return st->proof_requirements[i].public_id;
        }
    }
    return NULL;
}

service_object_t* auth_service_create(const char* policy_path)
{
    service_object_t* so = calloc(1, sizeof(*so));
    auth_service_state_t* st = calloc(1, sizeof(*st));
    if (!so || !st) {
        free(so);
        free(st);
        return NULL;
    }

    load_policy(st, policy_path);

    strncpy(so->path, "/lx/ob/so/aa/ac/cv", sizeof(so->path) - 1);
    so->state = st;
    so->on_get = auth_on_get;
    so->on_put = auth_on_put;
    so->tick = NULL;
    so->destroy = auth_destroy;
    return so;
}
