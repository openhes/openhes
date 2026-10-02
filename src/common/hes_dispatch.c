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
/// @brief Implementation of the Lexicon router (hes_dispatch.h): find the object
/// that owns an address, then hand the message to it.
///
/// @details
/// Two jobs, in order:
///
///   - find_object(): an exact address match wins; failing that, the LONGEST
///     registered address the path extends by exactly one segment -- which is
///     what lets "/lx/ob/so/id/co/st/cv/pi" reach the object at
///     "/lx/ob/so/id/co/st/cv" rather than the one at "/lx/ob/so/id".
///
///   - dispatch_to_local_objects(): call that object's service function with the
///     message, then trim a GET answer to the data points that were asked for
///     (the trailing datum, or the 'da' list). Both forms are handled here rather
///     than in each service, so every object gets them for free.
///
/// The payload convention is shared with the service objects: one record per
/// address, ';'-separated 'transCode=value' pairs -- the same shape the A&A
/// service's table answers use.

#include "hes_dispatch.h"

#include <log.h>

#include <stdio.h>
#include <string.h>

/// Payload convention shared with the service objects: one record per address,
/// as ';'-separated 'transCode=value' pairs (the A&A service's table answers use
/// the same shape). The 'da' query selects which of those pairs to keep.

////////////////////////////////////////////////////////////////////////////////
/// Does 'path' end exactly at 'prefix', or continue after a '/' boundary?
///
/// @param path The path to test.
/// @param prefix_len Length of the prefix to test against.
/// @return 1 when the path ends there or continues after a '/', 0 otherwise.
static int on_boundary(const char* path, size_t prefix_len)
{
    return path[prefix_len] == '\0' || path[prefix_len] == '/';
}

////////////////////////////////////////////////////////////////////////////////
/// True when 's' is a single path segment (no nested '/').
///
/// @param s The candidate segment.
/// @return 1 when it is exactly one segment, 0 otherwise.
static int single_segment(const char* s)
{
    return *s != '\0' && strchr(s, '/') == NULL;
}

////////////////////////////////////////////////////////////////////////////////
/// Finds the object an address belongs to.
///
/// An exact match wins. Failing that, the LONGEST registered address that the
/// path extends by exactly one segment wins -- which is what lets
/// "/lx/ob/so/id/co/st/cv/pi" reach the object at "/lx/ob/so/id/co/st/cv"
/// rather than the one at "/lx/ob/so/id".
///
/// @param objs The local service objects.
/// @param n_objs How many entries objs holds.
/// @param path The address to resolve.
/// @param selector_out Receives the trailing datum selector, or NULL when the
///        path matched an object exactly.
/// @return The object, or NULL when nothing owns the address.
static service_object_t* find_object(service_object_t** objs,
                                     int n_objs,
                                     const char* path,
                                     const char** selector_out)
{
    service_object_t* best = NULL;
    const char* best_selector = NULL;
    size_t best_len = 0;

    for (int i = 0; i < n_objs; i++) {
        size_t len = strlen(objs[i]->path);
        if (strncmp(path, objs[i]->path, len) != 0 || !on_boundary(path, len)) {
            continue;
        }

        const char* selector = NULL;
        if (path[len] == '/') {
            selector = path + len + 1;
            if (!single_segment(selector)) {
                continue;  // a nested address we do not serve
            }
        }

        if (best == NULL || len > best_len) {
            best = objs[i];
            best_selector = selector;
            best_len = len;
        }
    }

    *selector_out = best_selector;
    return best;
}

////////////////////////////////////////////////////////////////////////////////
/// Finds the 'da' list in a query string, or NULL when there is none.
///
/// The query is a '&'-separated list of selectors, e.g. "da=va,ne" or
/// "ei=1&da=va". Commas inside a 'da' list separate data points.
///
/// @param query The query string to scan.
/// @return A pointer to the list (the text after "da="), or NULL.
static const char* da_list(const char* query)
{
    const char* p = query;
    while ((p = strstr(p, "da=")) != NULL) {
        if (p == query || p[-1] == '&') {
            return p + 3;
        }
        p += 3;
    }
    return NULL;
}

////////////////////////////////////////////////////////////////////////////////
/// True when data point 'name' is in the request.
///
/// @param list The 'da' list to scan.
/// @param name The data point name to look for.
/// @param name_len Length of name.
/// @return 1 when the list names it, 0 otherwise.
static int requested(const char* list, const char* name, size_t name_len)
{
    const char* p = list;

    while (*p != '\0' && *p != '&') {
        while (*p == ',' || *p == ' ') {
            p++;  // tolerate "a, b" and a leading comma
        }

        const char* end = p;
        while (*end != '\0' && *end != ',' && *end != '&') {
            end++;
        }
        while (end > p && end[-1] == ' ') {
            end--;  // ... and the trailing space of "a , b"
        }

        if ((size_t)(end - p) == name_len && strncmp(p, name, name_len) == 0) {
            return 1;
        }

        p = end;
    }
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Trims a 'transCode=value;...' payload down to the requested data points.
///
/// The object's own order is kept (the order its tables list the data in), not
/// the order the client happened to name them -- the answer should not depend on
/// how the question was phrased.
///
/// An empty result is reported rather than passed off as a valid answer: either
/// the client named nothing (an empty 'da='), or it named data this object does
/// not have.
///
/// @param msg The message whose payload is trimmed in place.
/// @param list The 'da' list to keep.
static void trim_payload(hes_clme_msg_t* msg, const char* list)
{
    char kept[HES_PAYLOAD_MAX];
    const char* p = msg->payload;
    size_t used = 0;

    kept[0] = '\0';
    while (*p != '\0') {
        const char* end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        size_t name_len = 0;
        while (name_len < len && p[name_len] != '=') {
            name_len++;
        }

        if (requested(list, p, name_len)) {
            if (used + len + 2 > sizeof(kept)) {
                log_error("dispatch: answer to %s does not fit %d bytes", msg->path,
                          (int)sizeof(kept));
                break;
            }
            if (used > 0) {
                kept[used++] = ';';
            }
            memcpy(kept + used, p, len);
            used += len;
            kept[used] = '\0';
        }

        if (end == NULL) {
            break;
        }
        p = end + 1;
    }

    if (used == 0) {
        log_error("dispatch: no data named '%s' at %s", list, msg->path);
    }

    hes_msg_set_payload_str(msg, kept);
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

int dispatch_to_local_objects(hes_bus_t* bus,
                              service_object_t** objs,
                              int n_objs,
                              const hes_clme_msg_t* in)
{
    const char* selector = NULL;
    service_object_t* so = find_object(objs, n_objs, in->path, &selector);
    if (so == NULL) {
        return 0;
    }

    // Either addressing form ends up as one list of data points to keep. Using
    // both at once is a client error -- the address is the more specific of the
    // two, so it wins, and the query is reported rather than silently mixed in.
    const char* list;
    if (selector != NULL) {
        list = selector;
        if (da_list(in->query) != NULL) {
            log_error("%s already names a datum in the address; ignoring its 'da=' query",
                      in->path);
        }
    } else {
        list = da_list(in->query);
    }

    if (in->verb == HES_VERB_GET && so->on_get) {
        hes_clme_msg_t out = {0};
        out.verb = HES_VERB_EVENT;  // carries the answer back as a value
        hes_msg_set_path(&out, so->path);

        // Forward the incoming query/payload so objects that need to inspect the
        // request (e.g. crypto_service's "ei=" query, or a payload to transform)
        // can do so -- see the convention documented in service_object.h.
        hes_strlcpy(out.query, sizeof(out.query), in->query);
        out.payload_len = in->payload_len;
        memcpy(out.payload, in->payload, in->payload_len);

        so->on_get(so, &out);

        if (list != NULL) {
            trim_payload(&out, list);
        }

        hes_bus_send(bus, &out);
    } else if (in->verb == HES_VERB_PUT && so->on_put) {
        // Passed through unchanged, so the object sees the address it was
        // actually asked about (e.g. "/lx/ob/so/id/rq") and can report it.
        so->on_put(so, in);
    }
    return 1;
}
