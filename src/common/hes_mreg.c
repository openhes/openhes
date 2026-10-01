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
/// @brief Implementation of the Device Manifest service client (hes_mreg.h): the
/// REQ/REP queries and reports, and the SUB notification stream.
///
/// @details
/// Wire protocol (JSON strings over NNG; this is the "system" channel the
/// standard leaves to the manufacturer -- docs/poc_design.md section 9):
///
///   client -> service (REQ/REP):
///     query:  {"cmd":"query","moduleType":"hi","moduleRefIndex":N}
///             reply: the module's device slice (all its devices + status)
///     modules:{"cmd":"modules"}
///             reply: every module of this gateway (type, ref index, service
///                    domain, presence) -- the list the identification service
///                    counts for Table 42's nh/nw/ns
///     report: {"cmd":"report","moduleType":"hi","moduleRefIndex":N,
///              "devices":[{"deviceIndex":D,"netRefIndex":ni,
///                          "address":"...","udpPort":p,"online":1}, ...]}
///             reply: {"ok":1}
///
/// The module names itself by the pair (moduleType, moduleRefIndex); an empty
/// moduleType asks by index alone. See find_module() in the service.
///
///   service -> client (PUB/SUB, topic = moduleRefIndex):
///     message = "<moduleRefIndex>" + slice JSON, so each module receives
///     only its own devices.
///
/// Both sockets are dialed with a retry loop (50 attempts, 200 ms apart), so a
/// module can start before the service does.
///
/// Memory/ownership: the two nng sockets belong to the caller's hes_mreg_t;
/// nothing else is kept between calls. The JSON trees are Jansson's, and a slice
/// is copied into the caller's hes_devreg_t.
///
/// Threading: no locking -- the client is driven from the module's own loop.

#include "hes_mreg.h"

#include <jansson.h>
#include <log.h>
#include <nng/protocol/pubsub0/sub.h>
#include <nng/protocol/reqrep0/req.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HES_MREG_REQ_MAX 8192
#define HES_MREG_REPLY_MAX 65536

/// Default reply timeout: generous, because a module's boot-time query has
/// nothing else to do while it waits.
#define HES_MREG_REPLY_TIMEOUT_MS 3000

////////////////////////////////////////////////////////////////////////////////
/// Dials one endpoint, retrying so a module tolerates starting before the
/// service.
///
/// @param sock The socket to dial with.
/// @param url The endpoint to dial.
/// @return 0 once connected, or the last NNG error after the retries run out.
static int dial_retry(nng_socket sock, const char* url)
{
    int rv = 0;
    for (int attempt = 0; attempt < 50; attempt++) {
        rv = nng_dial(sock, url, NULL, 0);
        if (rv == 0) {
            return 0;
        }

        nng_msleep(200);
    }

    log_error("dial(%s) failed after retries: %s", url, nng_strerror(rv));
    return rv;
}

////////////////////////////////////////////////////////////////////////////////
/// One dial attempt. Used where the service is optional (the registry-only
/// handle): a missing service must not stall the caller for dial_retry's 10 s.
///
/// @param sock The socket to dial with.
/// @param url The endpoint to dial.
/// @return 0 once connected, or the NNG error from the single attempt.
static int dial_once(nng_socket sock, const char* url)
{
    int rv = nng_dial(sock, url, NULL, 0);
    if (rv != 0) {
        log_debug("dial(%s) failed: %s", url, nng_strerror(rv));
    }
    return rv;
}

////////////////////////////////////////////////////////////////////////////////
/// Sends one request and reads one reply (both NUL-terminated strings).
///
/// @param m The client handle to send on.
/// @param req The request text.
/// @param reply Receives the reply, NUL-terminated.
/// @param replysz Capacity of reply.
/// @return 0 on success, an NNG error code on failure.
static int req_reply(hes_mreg_t* m, const char* req, char* reply, size_t replysz)
{
    int rv = nng_send(m->req, (void*)req, strlen(req), 0);
    if (rv != 0) {
        log_error("send failed: %s", nng_strerror(rv));
        return rv;
    }

    size_t sz = replysz - 1;
    rv = nng_recv(m->req, reply, &sz, 0);
    if (rv != 0) {
        log_error("recv failed: %s", nng_strerror(rv));
        return rv;
    }

    reply[sz] = '\0';
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Parses a service reply into the caller's registry.
///
/// @param reg The registry to fill in.
/// @param json The reply text.
/// @return 0 on success, -1 on failure.
static int parse_slice(hes_devreg_t* reg, const char* json)
{
    json_error_t error;
    json_t* root = json_loads(json, 0, &error);
    if (!root) {
        log_error("cannot parse service reply: %s", error.text);
        return -1;
    }

    int rv = hes_devreg_parse(reg, root);
    json_decref(root);
    return rv;
}

/// Small readers for the module list's flat JSON objects.

////////////////////////////////////////////////////////////////////////////////
/// Copies a string member of a JSON object into a fixed-size buffer.
///
/// @param o The JSON object to read from.
/// @param key The member name.
/// @param dst Destination buffer.
/// @param dstsz Capacity of dst.
static void json_str_field(const json_t* o, const char* key, char* dst, size_t dstsz)
{
    const json_t* v = json_object_get(o, key);
    if (json_is_string(v)) {
        snprintf(dst, dstsz, "%s", json_string_value(v));
    }
}

////////////////////////////////////////////////////////////////////////////////
/// Reads an unsigned member of a JSON object, treating 0 as "absent".
///
/// @param o The JSON object to read from.
/// @param key The member name.
/// @return The value, or 0 when the member is missing, not a number or not
///         positive.
static uint32_t json_u32_field(const json_t* o, const char* key)
{
    const json_t* v = json_object_get(o, key);
    if (json_is_number(v) && json_number_value(v) > 0) {
        return (uint32_t)json_number_value(v);
    }
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Reads a 0/1 member of a JSON object.
///
/// @param o The JSON object to read from.
/// @param key The member name.
/// @return 1 when the member is a non-zero number, 0 otherwise.
static int json_flag_field(const json_t* o, const char* key)
{
    const json_t* v = json_object_get(o, key);
    return json_is_number(v) && json_number_value(v) != 0;
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

int hes_mreg_open(hes_mreg_t* m,
                  const char* svc_rep_url,
                  const char* svc_pub_url,
                  const char* module_type,
                  uint32_t module_ref_index)
{
    memset(m, 0, sizeof(*m));
    if (module_type != NULL) {
        snprintf(m->module_type, sizeof(m->module_type), "%s", module_type);
    }
    m->module_ref_index = module_ref_index;
    snprintf(m->topic, sizeof(m->topic), "%u", module_ref_index);

    int rv = nng_req0_open(&m->req);
    if (rv != 0) {
        log_error("req0_open failed: %s", nng_strerror(rv));
        return rv;
    }

    nng_socket_set_ms(m->req, NNG_OPT_RECVTIMEO, HES_MREG_REPLY_TIMEOUT_MS);
    if (dial_retry(m->req, svc_rep_url) != 0) {
        nng_close(m->req);
        return -1;
    }

    rv = nng_sub0_open(&m->sub);
    if (rv != 0) {
        log_error("sub0_open failed: %s", nng_strerror(rv));
        nng_close(m->req);
        return rv;
    }

    nng_socket_set(m->sub, NNG_OPT_SUB_SUBSCRIBE, m->topic, strlen(m->topic));
    if (dial_retry(m->sub, svc_pub_url) != 0) {
        nng_close(m->req);
        nng_close(m->sub);
        return -1;
    }

    return 0;
}

int hes_mreg_open_registry(hes_mreg_t* m, const char* svc_rep_url, int timeout_ms)
{
    if (m == NULL || svc_rep_url == NULL || svc_rep_url[0] == '\0') {
        return -1;
    }

    memset(m, 0, sizeof(*m));

    int rv = nng_req0_open(&m->req);
    if (rv != 0) {
        log_error("req0_open failed: %s", nng_strerror(rv));
        return rv;
    }

    nng_socket_set_ms(m->req, NNG_OPT_RECVTIMEO,
                      (timeout_ms > 0) ? timeout_ms : HES_MREG_REPLY_TIMEOUT_MS);
    if (dial_once(m->req, svc_rep_url) != 0) {
        nng_close(m->req);
        memset(m, 0, sizeof(*m));
        return -1;
    }

    return 0;
}

int hes_mreg_query(hes_mreg_t* m, hes_devreg_t* reg)
{
    // Say which module we are: (moduleType, moduleRefIndex) is the identity, and
    // the index alone does not say which slice is meant -- a profile regularly
    // holds a service module ('sm') and an interface module ('hi') at the same
    // index, and the wrong one answers with no devices.
    char req[HES_MREG_REQ_MAX] = {0};
    snprintf(req, sizeof(req), "{\"cmd\":\"query\",\"moduleType\":\"%s\",\"moduleRefIndex\":%u}",
             m->module_type, m->module_ref_index);

    char reply[HES_MREG_REPLY_MAX] = {0};
    if (req_reply(m, req, reply, sizeof(reply)) != 0) {
        return -1;
    }

    return parse_slice(reg, reply);
}

int hes_mreg_query_modules(hes_mreg_t* m, hes_modules_t* out)
{
    if (m == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    char req[HES_MREG_REQ_MAX] = {0};
    snprintf(req, sizeof(req), "{\"cmd\":\"modules\"}");

    char reply[HES_MREG_REPLY_MAX] = {0};
    if (req_reply(m, req, reply, sizeof(reply)) != 0) {
        return -1;
    }

    json_error_t error;
    json_t* root = json_loads(reply, 0, &error);
    if (!root) {
        log_error("cannot parse the module list: %s", error.text);
        return -1;
    }

    const json_t* arr = json_object_get(root, "modules");
    int rv = -1;
    if (json_is_array(arr)) {
        size_t n = json_array_size(arr);
        for (size_t i = 0; i < n && out->n < HES_MODULES_MAX; i++) {
            const json_t* mo = json_array_get(arr, i);
            if (!json_is_object(mo)) {
                continue;
            }

            hes_module_entry_t* mod = &out->modules[out->n];
            json_str_field(mo, "moduleType", mod->module_type, sizeof(mod->module_type));
            json_str_field(mo, "serviceModuleType", mod->service_module_type,
                           sizeof(mod->service_module_type));
            mod->module_ref_index = json_u32_field(mo, "moduleRefIndex");
            mod->present = json_flag_field(mo, "present");
            out->n++;
        }
        rv = 0;
    }

    json_decref(root);
    return rv;
}

int hes_mreg_report(hes_mreg_t* m, const hes_devreg_t* reg)
{
    char req[HES_MREG_REQ_MAX] = {0};
    size_t off = (size_t)snprintf(
            req, sizeof(req),
            "{\"cmd\":\"report\",\"moduleType\":\"%s\",\"moduleRefIndex\":%u,\"devices\":[",
            m->module_type, m->module_ref_index);
    for (int i = 0; i < reg->n_devices && off < sizeof(req); i++) {
        const hes_device_t* d = &reg->devices[i];
        int n = snprintf(req + off, sizeof(req) - off,
                         "%s{\"deviceIndex\":%u,\"netRefIndex\":%u,"
                         "\"address\":\"%s\",\"udpPort\":%d,\"online\":%d}",
                         (i ? "," : ""), d->device_index, d->net_ref_index, d->address, d->udp_port,
                         d->online ? 1 : 0);
        if (n < 0) {
            return -1;
        }
        off += (size_t)n;
    }
    snprintf(req + off, sizeof(req) - off, "]}");

    char reply[256] = {0};
    return req_reply(m, req, reply, sizeof(reply)) == 0 ? 0 : -1;
}

int hes_mreg_recv_update(hes_mreg_t* m, hes_devreg_t* reg, int timeout_ms)
{
    nng_duration d = (timeout_ms < 0) ? NNG_DURATION_INFINITE : (nng_duration)timeout_ms;
    nng_socket_set_ms(m->sub, NNG_OPT_RECVTIMEO, d);

    char buf[HES_MREG_REPLY_MAX] = {0};
    size_t sz = sizeof(buf) - 1;
    int rv = nng_recv(m->sub, buf, &sz, 0);
    if (rv != 0) {
        return rv;  // e.g. NNG_ETIMEDOUT
    }
    buf[sz] = '\0';

    // strip our own topic prefix (guaranteed by the subscription)
    const char* p = buf;
    size_t tlen = strlen(m->topic);
    if (strncmp(p, m->topic, tlen) == 0) {
        p += tlen;
    }

    return parse_slice(reg, p);
}

void hes_mreg_close(hes_mreg_t* m)
{
    // A registry-only handle (hes_mreg_open_registry) has no notification
    // socket, so close only what was actually opened.
    if (nng_socket_id(m->req) >= 0) {
        nng_close(m->req);
    }
    if (nng_socket_id(m->sub) >= 0) {
        nng_close(m->sub);
    }
}
