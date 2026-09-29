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
/// @brief Implementation of the device registry (hes_devreg.h): the slice JSON
/// schema mapped onto hes_devreg_t, plus the device/object lookups.
///
/// @details
/// The slice used to be a file (dev_manifest.json); it now normally arrives over
/// NNG from the Device Manifest service, so the parsing is split in two:
///
///   - hes_devreg_parse(): populate from an already-parsed JSON tree -- the path
///     the manifest client uses.
///
///   - hes_devreg_load_file(): read a file first (legacy/bootstrap).
///
/// Memory/ownership: writes only into the caller's hes_devreg_t; the JSON tree
/// stays Jansson's. Strings are copied in with a bounded copy, so nothing here
/// borrows from the document.
///
/// Threading: no shared state.

#include "hes_devreg.h"

#include "utils.h"

#include <log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

////////////////////////////////////////////////////////////////////////////////
/// Bounded string copy that always NUL-terminates.
///
/// @param dst Destination buffer.
/// @param dstsz Capacity of dst.
/// @param src Source string.
/// @return The source length (may exceed dst capacity if truncated).
static size_t copy_str(char* dst, size_t dstsz, const char* src)
{
    size_t n = strlen(src);
    if (n >= dstsz) {
        n = dstsz - 1;
    }

    memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}

////////////////////////////////////////////////////////////////////////////////
/// Copies a string member of a JSON object, when it is present and a string.
///
/// @param obj The JSON object to read from.
/// @param key The member name.
/// @param dst Destination buffer.
/// @param dstsz Capacity of dst.
/// @return 0 when the member was a string and was copied, -1 otherwise.
static int get_member_string(const json_t* obj, const char* key, char* dst, size_t dstsz)
{
    const json_t* m = json_object_get(obj, key);
    if (!json_is_string(m)) {
        return -1;
    }

    copy_str(dst, dstsz, json_string_value(m));
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Reads a numeric member of a JSON object.
///
/// @param obj The JSON object to read from.
/// @param key The member name.
/// @param out Receives the value.
/// @return 0 when the member was a number and was read, -1 otherwise.
static int get_member_long(const json_t* obj, const char* key, long* out)
{
    const json_t* m = json_object_get(obj, key);
    if (!json_is_number(m)) {
        return -1;
    }

    *out = (long)json_number_value(m);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// A device is online unless the slice explicitly says otherwise.
///
/// @param dev The device entry from the slice.
/// @return 1 when online, 0 when the slice says otherwise.
static int status_to_online(const json_t* dev)
{
    const json_t* s = json_object_get(dev, "status");
    if (!json_is_string(s)) {
        return 1;  // absent status (legacy slices) -> online
    }

    return strcmp(json_string_value(s), "online") == 0;
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

void hes_devreg_init(hes_devreg_t* reg)
{
    memset(reg, 0, sizeof(*reg));
}

int hes_devreg_parse(hes_devreg_t* reg, const json_t* root)
{
    hes_devreg_init(reg);
    if (!json_is_object(root)) {
        log_error("device slice is not an object");
        return -1;
    }

    // A service that cannot satisfy the request answers {"error":"..."}. Say so:
    // without this text the caller can only report "no device slice" and the
    // reason (e.g. "no module hi/1") is lost.
    const json_t* err = json_object_get(root, "error");
    if (json_is_string(err)) {
        log_error("manifest service refused: %s", json_string_value(err));
        return -1;
    }

    get_member_string(root, "gatewayId", reg->gateway_id, sizeof(reg->gateway_id));
    get_member_string(root, "moduleType", reg->module_type, sizeof(reg->module_type));
    get_member_string(root, "networkType", reg->network_type, sizeof(reg->network_type));
    long mi = 0;
    if (get_member_long(root, "moduleRefIndex", &mi) == 0) {
        reg->module_ref_index = (uint32_t)mi;
    }

    const json_t* devices = json_object_get(root, "devices");
    if (!json_is_array(devices)) {
        log_error("missing \"devices\" array");
        return -1;
    }

    int n_dev = 0;
    const size_t nd = json_array_size(devices);
    for (size_t i = 0; i < nd && n_dev < HES_DEVREG_MAX_DEVICES; i++) {
        const json_t* d = json_array_get(devices, i);
        if (!json_is_object(d)) {
            continue;
        }

        hes_device_t* dev = &reg->devices[n_dev];
        memset(dev, 0, sizeof(*dev));

        long v = 0;
        if (get_member_long(d, "deviceIndex", &v) != 0) {
            log_error("device[%zu] missing deviceIndex", i);
            continue;
        }
        dev->device_index = (uint32_t)v;

        if (get_member_long(d, "netRefIndex", &v) != 0) {
            v = 0;
        }
        dev->net_ref_index = (uint32_t)v;

        get_member_string(d, "deviceRole", dev->role, sizeof(dev->role));
        get_member_string(d, "expectedHardware", dev->expected_hardware,
                          sizeof(dev->expected_hardware));
        get_member_string(d, "networkType", dev->network_type, sizeof(dev->network_type));
        if (get_member_long(d, "udpPort", &v) == 0) {
            dev->udp_port = (int)v;
        }
        dev->online = status_to_online(d);

        const json_t* addr = json_object_get(d, "address");
        if (json_is_string(addr)) {
            copy_str(dev->address, sizeof(dev->address), json_string_value(addr));
        }

        // object list
        const json_t* objects = json_object_get(d, "objects");
        if (json_is_array(objects)) {
            const size_t no = json_array_size(objects);
            for (size_t k = 0; k < no && dev->n_objects < HES_DEVREG_MAX_OBJECTS; k++) {
                const json_t* o = json_array_get(objects, k);
                if (!json_is_object(o)) {
                    continue;
                }
                hes_dev_object_t* obj = &dev->objects[dev->n_objects];
                if (get_member_string(o, "lexiconPath", obj->path, sizeof(obj->path)) != 0) {
                    continue;
                }
                if (get_member_string(o, "access", obj->access, sizeof(obj->access)) != 0) {
                    continue;
                }
                dev->n_objects++;
            }
        }

        if (reg->network_type[0] == '\0' && dev->network_type[0] != '\0') {
            copy_str(reg->network_type, sizeof(reg->network_type), dev->network_type);
        }

        fprintf(stderr, "hes_devreg: device di=%u ni=%u role=%s hw=%s addr=%s %s (%d object(s))\n",
                dev->device_index, dev->net_ref_index, dev->role, dev->expected_hardware,
                dev->address[0] ? dev->address : "-", dev->online ? "online" : "offline",
                dev->n_objects);
        n_dev++;
    }
    reg->n_devices = n_dev;
    // An empty device list is a SUCCESS, not a parse failure. "This module fronts
    // no devices" is a legitimate answer to a slice query, and only the caller
    // knows whether it is the configuration error worth refusing to start over
    // (the interface modules say exactly that). Returning a failure here is what
    // used to hide the reason behind a bare "no device slice".
    return 0;
}

int hes_devreg_load_file(hes_devreg_t* reg, const char* json_path)
{
    char* text = read_file(json_path);
    if (text == NULL) {
        log_error("failed to load device manifest: %s", json_path);
        return -1;
    }

    json_error_t error;
    json_t* root = json_loads(text, 0, &error);
    free(text);
    if (!root) {
        log_error("cannot parse '%s': %s", json_path, error.text);
        return -1;
    }

    int rv = hes_devreg_parse(reg, root);
    json_decref(root);
    return rv;
}

const hes_device_t* hes_devreg_find(const hes_devreg_t* reg, uint32_t device_index)
{
    for (int i = 0; i < reg->n_devices; i++) {
        if (reg->devices[i].device_index == device_index) {
            return &reg->devices[i];
        }
    }

    return NULL;
}

const hes_dev_object_t* hes_dev_object_find(const hes_device_t* dev, const char* path)
{
    for (int i = 0; i < dev->n_objects; i++) {
        if (strcmp(dev->objects[i].path, path) == 0) {
            return &dev->objects[i];
        }
    }

    return NULL;
}

int hes_dev_object_can(const hes_device_t* dev, const char* path, char access)
{
    const hes_dev_object_t* o = hes_dev_object_find(dev, path);
    return o && strchr(o->access, access) != NULL;
}

int hes_devreg_collect_topics(const hes_devreg_t* reg, char topics[][HES_PATH_MAX], int max)
{
    int n = 0;
    for (int i = 0; i < reg->n_devices && n < max; i++) {
        const hes_device_t* dev = &reg->devices[i];
        for (int j = 0; j < dev->n_objects && n < max; j++) {
            // dedup across devices sharing the same object path
            int dup = 0;
            for (int k = 0; k < n; k++) {
                if (strcmp(topics[k], dev->objects[j].path) == 0) {
                    dup = 1;
                    break;
                }
            }

            if (!dup) {
                copy_str(topics[n], HES_PATH_MAX, dev->objects[j].path);
                n++;
            }
        }
    }

    return n;
}
