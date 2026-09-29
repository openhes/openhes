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
/// @brief The WiFi HAN interface module: Philips WiZ A19 bulbs over WiFi (UDP),
/// ISO/IEC 15045-4-1 Annex A.1.
///
/// @details
/// Same A.1 obligations as the BLE module (see src/han/ble/ble.c):
/// translate between HES-CLME and the HAN device, send only to the
/// binding map, accept only from the binding map / for our own
/// serviceData.
///
/// Device info comes from the Device Manifest service (common/hes_mreg.h)
/// over NNG, NOT from a file: at boot the module queries its slice (only
/// the devices IT manages), reports its real devices (bulb IP/port), then
/// subscribes to notifications so it can take devices offline/online live
/// (e.g. when a bulb stops answering getPilot).
///
/// Per device it:
///   - turns PUT messages on its owned object path into real WiZ UDP
///     setPilot commands (write objects, access 'w');
///   - periodically polls the bulb's actual state and republishes it as an
///     event-report stamped with the device's deviceIndex (read objects,
///     access 'r'), so the gateway sees ground truth per bulb.
///
/// Memory/ownership: the module's state is local to han_wifi_main() -- the device
/// registry, one wiz_client_t per device and the bus handle. The configuration
/// strings are the caller's.
///
/// Threading: one loop. It blocks in hes_bus_recv() for a bounded timeout, drains
/// the manifest notifications, polls each bulb and republishes changed state;
/// SIGINT/SIGTERM only clear the running flag.

#include "wifi.h"

#include "app_config.h"
#include "common/hes_bus.h"
#include "common/hes_common.h"
#include "common/hes_devreg.h"
#include "common/hes_mreg.h"
#include "wiz_client.h"

#include <log.h>

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_running = 1;
////////////////////////////////////////////////////////////////////////////////
/// SIGINT/SIGTERM handler: asks the main loop to stop by clearing g_running.
/// It does nothing else on purpose -- the loop then closes the devices and the
/// bus in order, which a signal handler could not do safely.
///
/// @param sig The signal number (unused).
static void on_sigint(int sig)
{
    (void)sig;
    g_running = 0;
}

#define POLL_PERIOD_SEC 10
#define DEFAULT_BULB_IP "192.168.1.142"

////////////////////////////////////////////////////////////////////////////////
/// One opened device: index into the registry + its UDP client.
typedef struct wifi_dev {
    int reg_idx;
    wiz_client_t sock;
} wifi_dev_t;

////////////////////////////////////////////////////////////////////////////////
/// Interprets a binding-map PUT payload as a boolean light state: "1", "on" or
/// "true" mean on, anything else means off.
///
/// @param payload The PUT payload, as received on the bus.
/// @return true when the payload asks for the light to be on.
static bool payload_to_bool(const char* payload)
{
    if (!strcmp(payload, "1") || !strcmp(payload, "on") || !strcmp(payload, "true")) {
        return true;
    }
    return false;
}

////////////////////////////////////////////////////////////////////////////////
/// Looks a device up by the deviceIndex ('di') that the bus and the binding map
/// use for it.
///
/// @param reg The registry to search.
/// @param di  The deviceIndex to find.
/// @return The registry index, or -1 when no device carries that index.
static int index_by_di(const hes_devreg_t* reg, uint32_t di)
{
    for (int i = 0; i < reg->n_devices; i++) {
        if (reg->devices[i].device_index == di) {
            return i;
        }
    }
    return -1;
}

////////////////////////////////////////////////////////////////////////////////
/// Resolves which of our devices a received message addresses.
///
/// A message normally carries the deviceIndex ('di') the binding map assigned,
/// and the device it names must be one we front, be online and expose the
/// message's object path. A message with no deviceIndex (0) is attributed to
/// the only online device exposing that path, and refused when two or more do:
/// identical devices share their object paths, so a guess would command the
/// wrong one.
///
/// @param reg  The registry of the devices this module fronts.
/// @param path The object path the message is addressed to.
/// @param di   The message's deviceIndex, or 0 when it carries none.
/// @return The registry index of the addressed device, or -1 when no device --
///         or no unambiguous one -- matches.
static int resolve_index(const hes_devreg_t* reg, const char* path, uint32_t di)
{
    int idx = index_by_di(reg, di);
    if (idx >= 0 && reg->devices[idx].online && hes_dev_object_find(&reg->devices[idx], path)) {
        return idx;
    }
    if (di != 0) {
        return -1;
    }
    int found = -1;
    for (int i = 0; i < reg->n_devices; i++) {
        if (reg->devices[i].online && hes_dev_object_find(&reg->devices[i], path)) {
            if (found >= 0) {
                return -1;  // ambiguous
            }
            found = i;
        }
    }
    return found;
}

////////////////////////////////////////////////////////////////////////////////
/// Finds the already-opened socket that serves a registry entry, so a device is
/// dialled once and then reused.
///
/// @param devs    The table of opened devices.
/// @param n       How many entries it holds.
/// @param reg_idx The registry index to look for.
/// @return The table index, or -1 when that device has no open socket yet.
static int find_open(wifi_dev_t* devs, int n, int reg_idx)
{
    for (int i = 0; i < n; i++) {
        if (devs[i].reg_idx == reg_idx) {
            return i;
        }
    }
    return -1;
}

////////////////////////////////////////////////////////////////////////////////
/// Reads one object's current value from the device into buf, stringified for
/// the bus.
///
/// The object's path selects the handler: the light's power state and its
/// brightness are the two readable objects, and each is a separate question to
/// the device. The value is formatted as the bare decimal an interface module
/// publishes (no 'transCode=value' records).
///
/// @param w     The opened device to read from.
/// @param obj   The object to read; its path selects the handler.
/// @param buf   Receives the value as a NUL-terminated string.
/// @param bufsz The capacity of buf.
/// @return 0 on success, -1 when no handler covers this path or the device did
///         not answer.
static int read_object(wifi_dev_t* w, const hes_dev_object_t* obj, char* buf, size_t bufsz)
{
    if (strcmp(obj->path, HES_LX_LIGHT_CV) == 0) {
        bool on;
        if (wiz_client_get_state(&w->sock, &on) != 0) {
            return -1;
        }
        snprintf(buf, bufsz, "%d", on ? 1 : 0);
        return 0;
    }
    if (strcmp(obj->path, HES_LX_LIGHT_BRIGHTNESS_CV) == 0) {
        int level;
        if (wiz_client_get_brightness(&w->sock, &level) != 0) {
            return -1;
        }
        snprintf(buf, bufsz, "%d", level);
        return 0;
    }
    return -1;  // no handler for this object path
}

////////////////////////////////////////////////////////////////////////////////
/// Applies a PUT payload to one object on the device -- the write side of
/// read_object(); the object's path selects the handler.
///
/// @param w       The opened device to write to.
/// @param obj     The object to write; its path selects the handler.
/// @param payload The PUT payload, as received on the bus.
/// @return 0 on success, -1 when no handler covers this path or the device did
///         not answer.
static int write_object(wifi_dev_t* w, const hes_dev_object_t* obj, const char* payload)
{
    if (strcmp(obj->path, HES_LX_LIGHT_CV) == 0) {
        return wiz_client_set_state(&w->sock, payload_to_bool(payload));
    }
    if (strcmp(obj->path, HES_LX_LIGHT_BRIGHTNESS_CV) == 0) {
        // Brightness is an intensity, 0-100 (18012-3 'va'). Setting it
        // implies the bulb should be lit: level 0 turns it off, a positive
        // level turns it on and dims to that percentage (the WiZ helper
        // preserves the current hue while rescaling the drive level).
        int level = atoi(payload);
        if (level <= 0) {
            return wiz_client_set_state(&w->sock, false);
        }
        wiz_client_set_state(&w->sock, true);
        return wiz_client_set_brightness(&w->sock, level);
    }
    return -1;
}

////////////////////////////////////////////////////////////////////////////////
/// Merges one manifest slice into the running registry: for every device we
/// already know, copies its online flag (logging a transition) and its address.
///
/// A device the slice does not mention keeps its current state -- a slice is a
/// part of the gateway's inventory, not the whole of it.
///
/// @param reg The registry to update.
/// @param upd The slice just received from the manifest service.
static void merge_slice(hes_devreg_t* reg, const hes_devreg_t* upd)
{
    for (int i = 0; i < reg->n_devices; i++) {
        int j = index_by_di(upd, reg->devices[i].device_index);
        if (j < 0) {
            continue;
        }
        if (reg->devices[i].online != upd->devices[j].online) {
            log_info("wifi: device di=%u now %s", reg->devices[i].device_index,
                     upd->devices[j].online ? "online" : "offline");
        }
        reg->devices[i].online = upd->devices[j].online;
        if (upd->devices[j].address[0]) {
            snprintf(reg->devices[i].address, sizeof(reg->devices[i].address), "%s",
                     upd->devices[j].address);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

int han_wifi_main(const char* svc_rep,
                  const char* svc_pub,
                  uint32_t module_ref,
                  const char* hub_pub_url,
                  const char* hub_sub_url,
                  const char* bulb_ip)
{
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    log_info("manifest service at rep=%s pub=%s (moduleRefIndex=%u)", svc_rep, svc_pub, module_ref);

    hes_mreg_t mreg = {0};
    // HES_MT_INTERFACE is this module's 'mt': (moduleType, moduleRefIndex) is the
    // identity the service matches a slice request against (see hes_mreg.h).
    int rv = hes_mreg_open(&mreg, svc_rep, svc_pub, HES_MT_INTERFACE, module_ref);
    if (rv != 0) {
        log_error("cannot reach manifest service");
        return 1;
    }

    hes_devreg_t reg = {0};
    rv = hes_mreg_query(&mreg, &reg);
    if (rv != 0) {
        log_error("no device slice from manifest service");
        return 1;
    }

    // 0 assigned devices = profile / (moduleType, moduleRefIndex) config error,
    // NOT a connectivity state (a bulb that is switched off only clears
    // `online`). Fail fast with the reason instead of coming up managing nothing.
    if (reg.n_devices == 0) {
        log_error(
                "no devices assigned to moduleRefIndex %u by the product profile; check "
                "the profile and module_ref (config error, not a connection problem)",
                module_ref);
        return 1;
    }

    // We manage every device the profile assigns to our moduleRefIndex.
    // Assign the real bulb address (discovered/configured here) and report
    // so the service registry + subscribers are up to date.
    for (int i = 0; i < reg.n_devices; i++) {
        const hes_dev_object_t* dev = hes_dev_object_find(&reg.devices[i], HES_LX_LIGHT_CV);
        if (dev) {
            if (reg.devices[i].address[0] == '\0') {
                snprintf(reg.devices[i].address, sizeof(reg.devices[i].address), "%s", bulb_ip);
            }

            if (reg.devices[i].udp_port <= 0) {
                reg.devices[i].udp_port = 38899;
            }

            reg.devices[i].online = 1;
        }
    }

    if (hes_mreg_report(&mreg, &reg) != 0) {
        log_warn("failed to report wiz device");
    }

    // Re-query: adopt the service's normalized view of our report (bound/online
    // status + the stored address/port) before we open any socket.
    hes_devreg_t reg2 = {0};
    rv = hes_mreg_query(&mreg, &reg2);
    if (rv != 0) {
        log_error("no device slice from device manifest service after report");
        return 1;
    }
    reg = reg2;

    // Open one UDP client per ONLINE device that has an address.
    wifi_dev_t devs[HES_DEVREG_MAX_DEVICES];
    int n_devs = 0;
    for (int i = 0; i < reg.n_devices; i++) {
        if (!reg.devices[i].online || reg.devices[i].address[0] == '\0' ||
            reg.devices[i].udp_port <= 0) {
            continue;
        }

        wifi_dev_t* w = &devs[n_devs];
        memset(w, 0, sizeof(*w));
        w->reg_idx = i;
        if (wiz_client_open(&w->sock, reg.devices[i].address, reg.devices[i].udp_port) != 0) {
            log_error("failed to open WiZ client for di=%u (%s:%d)", reg.devices[i].device_index,
                      reg.devices[i].address, reg.devices[i].udp_port);
            continue;
        }

        log_info("device di=%u ni=%u -> %s:%d", reg.devices[i].device_index,
                 reg.devices[i].net_ref_index, reg.devices[i].address, reg.devices[i].udp_port);
        n_devs++;
    }

    if (n_devs == 0) {
        log_warn("no online devices to serve");
        return 1;
    }

    // Subscribe to the union of object paths across all our devices.
    char topicbuf[HES_BUS_MAX_TOPICS][HES_PATH_MAX];
    const char* topics[HES_BUS_MAX_TOPICS];
    int n_topics = hes_devreg_collect_topics(&reg, topicbuf, HES_BUS_MAX_TOPICS);
    for (int i = 0; i < n_topics; i++) {
        topics[i] = topicbuf[i];
    }

    log_info("wifi: dialing core module (sub<-%s, pub->%s)", hub_pub_url, hub_sub_url);
    hes_bus_t bus = {0};
    if (hes_bus_leaf_open(&bus, hub_pub_url, hub_sub_url, topics, n_topics) != 0) {
        log_error("wifi: failed to open bus leaf");
        return 1;
    }

    log_info("wifi: connected; %d device(s), %d topic(s). Ctrl-C to stop.", n_devs, n_topics);

    time_t last_poll = 0;
    while (g_running) {
        hes_clme_msg_t in = {0};
        // Block for at most 200 ms (timeout_ms).
        int rv = hes_bus_recv(&bus, &in, 200);
        if (rv == 0) {
            if (in.verb == HES_VERB_SUBSCRIBE) {
                for (int i = 0; i < reg.n_devices; i++) {
                    if (reg.devices[i].online && hes_dev_object_find(&reg.devices[i], in.path)) {
                        log_info("wifi: subscription confirmed for %s (di=%u)", in.path,
                                 reg.devices[i].device_index);
                    }
                }
            } else if (in.verb == HES_VERB_PUT) {
                int ridx = resolve_index(&reg, in.path, in.device_index);
                int didx = ridx >= 0 ? find_open(devs, n_devs, ridx) : -1;
                const hes_dev_object_t* obj =
                        (ridx >= 0) ? hes_dev_object_find(&reg.devices[ridx], in.path) : NULL;
                if (didx >= 0 && obj && strchr(obj->access, 'w')) {
                    log_info("wifi: PUT %s di=%u = %s -> setPilot", in.path,
                             reg.devices[ridx].device_index, in.payload);
                    if (write_object(&devs[didx], obj, in.payload) != 0) {
                        log_error("wifi: write to %s di=%u FAILED", in.path,
                                  reg.devices[ridx].device_index);
                    }
                }
            } else if (in.verb == HES_VERB_GET) {
                int ridx = resolve_index(&reg, in.path, in.device_index);
                int didx = ridx >= 0 ? find_open(devs, n_devs, ridx) : -1;
                const hes_dev_object_t* obj =
                        (ridx >= 0) ? hes_dev_object_find(&reg.devices[ridx], in.path) : NULL;
                if (didx >= 0 && obj && strchr(obj->access, 'r')) {
                    char val[32];
                    if (read_object(&devs[didx], obj, val, sizeof(val)) == 0) {
                        hes_clme_msg_t out = {0};
                        out.verb = HES_VERB_EVENT;
                        out.device_index = reg.devices[ridx].device_index;
                        hes_msg_set_path(&out, in.path);
                        hes_msg_set_payload_str(&out, val);
                        hes_bus_send(&bus, &out);
                    }
                }
            }
        }

        // Apply any manifest-service notification.
        hes_devreg_t upd;
        if (hes_mreg_recv_update(&mreg, &upd, 0) == 0) {
            log_info("wifi: manifest notification received");
            merge_slice(&reg, &upd);
        }

        // Periodically republish ground truth per online device; track real
        // presence (a bulb that stops answering is reported offline).
        time_t now = time(NULL);
        if (now - last_poll >= POLL_PERIOD_SEC) {
            last_poll = now;
            for (int i = 0; i < n_devs; i++) {
                int ridx = devs[i].reg_idx;
                if (!reg.devices[ridx].online) {
                    continue;
                }
                int reachable = 0;
                for (int k = 0; k < reg.devices[ridx].n_objects; k++) {
                    const hes_dev_object_t* obj = &reg.devices[ridx].objects[k];
                    if (!strchr(obj->access, 'r')) {
                        continue;
                    }
                    char val[32];
                    if (read_object(&devs[i], obj, val, sizeof(val)) == 0) {
                        reachable = 1;
                        hes_clme_msg_t msg = {0};
                        msg.verb = HES_VERB_EVENT;
                        msg.device_index = reg.devices[ridx].device_index;
                        hes_msg_set_path(&msg, obj->path);
                        strncpy(msg.query, "va", sizeof(msg.query) - 1);
                        hes_msg_set_payload_str(&msg, val);
                        hes_bus_send(&bus, &msg);
                        log_info("wifi: EVENT %s di=%u = %s (polled)", obj->path,
                                 reg.devices[ridx].device_index, val);
                    }
                }
                // presence transition -> report to manifest service
                if (!reachable && reg.devices[ridx].online) {
                    reg.devices[ridx].online = 0;
                    log_info("wifi: device di=%u not reachable -> offline, reporting",
                             reg.devices[ridx].device_index);
                    hes_mreg_report(&mreg, &reg);
                }
            }
        }
    }

    log_info("wifi: shutting down");
    for (int i = 0; i < n_devs; i++) {
        wiz_client_close(&devs[i].sock);
    }
    hes_mreg_close(&mreg);
    hes_bus_close(&bus);
    return 0;
}
