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
/// @brief The BLE HAN interface module: TI SensorTag CC2650 nodes over BLE
/// (ISO/IEC 15045-4-1 Annex A.1).
///
/// @details
/// Per A.1, a HAN interface module shall:
///
///   - interface to the event bus using HES-CLME;
///   - interface to the HAN (here: BLE / SensorTag);
///   - translate between the two, using Lexicon user objects;
///   - send HES-CLME messages only to the binding map service;
///   - accept incoming HES-CLME only from the binding map service, or from
///     service modules for its own serviceData.
///
/// Device info comes from the Device Manifest service (common/hes_mreg.h) over
/// NNG, NOT from a file: at boot the module queries its slice (only the devices IT
/// manages), reports its real/simulated devices, and then subscribes to
/// notifications so it can take devices offline/online live. Every inbound message
/// is attributed to a device by deviceIndex, and messages for devices not in our
/// slice are ignored.
///
/// It periodically reads each device's read objects and publishes an event-report
/// stamped with that device's deviceIndex.
///
/// NOTE: presence and the button are real when built with BLE_BACKEND=bluez (see
/// hes_ble_bluez.c); all other sensor reads remain simulated -- see
/// src/devices/ti_cc2650/sensortag.h for the real GATT UUIDs and where to swap
/// them in.
///
/// Memory/ownership: the module's state is local to han_ble_main() -- the device
/// registry, the per-device contexts and the bus handle. The service accessors it
/// calls live in the HES-free device layer, src/devices/ti_cc2650/.
///
/// Threading: one loop. It blocks in hes_bus_recv() for at most
/// BUS_RECV_TIMEOUT_MS, drains the manifest notifications and polls the BLE
/// manager on each pass; SIGINT/SIGTERM only clear a flag.

#include "ble.h"

#include "app_config.h"
#include "common/hes_bus.h"
#include "common/hes_common.h"
#include "common/hes_devreg.h"
#include "common/hes_mreg.h"
#include "devices/ti_cc2650/sensortag.h"
#include "hes_ble.h"

#include <log.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_running = 1;

////////////////////////////////////////////////////////////////////////////////
/// SIGINT/SIGTERM handler: asks the main loop to stop by clearing g_running.
/// It does nothing else on purpose -- the loop then closes the BLE manager and
/// the bus in order, which a signal handler could not do safely.
///
/// @param sig The signal number (unused).
static void on_sigint(int sig)
{
    (void)sig;
    g_running = 0;
}

#define PUBLISH_PERIOD_SEC 5

/// Idle poll cadence of the main loop: how long hes_bus_recv() blocks when no
/// bus message is pending. This bounds how quickly the loop wakes to drain the
/// bluez D-Bus button notify (hes_ble_mgr_poll, step 3) and report presence --
/// the lower it is, the snappier the button feels. Too low just wastes CPU
/// spinning when idle (the downstream core/wifi hops are message-driven, so
/// only this module's poll cadence is governed by it).
#define BUS_RECV_TIMEOUT_MS 100

////////////////////////////////////////////////////////////////////////////////
/// One live SensorTag context; devs[i] corresponds to reg.devices[i].
typedef struct ble_dev {
    sensortag_t tag;  ///< The simulated SensorTag context for this device.
    int btn_set;      ///< have we seen/published a button value yet?
    int last_btn;     ///< last button value we published (1 pressed / 0)
} ble_dev_t;

////////////////////////////////////////////////////////////////////////////////
/// Resolves which device a received message addresses, requiring it to be
/// one of OURS and online.
///
/// @param reg The module's device registry (slice)
/// @param di The deviceIndex of the message (0 if not specified)
/// @return The index of the device in reg.devices[] if found and online, -1 otherwise
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
/// Resolves which device a received message addresses, requiring it to be
/// one of OURS and online.
///
/// @param reg The module's device registry (slice)
/// @param path The object path of the message
/// @param di The deviceIndex of the message (0 if not specified)
/// @return The index of the device in reg.devices[] if found and online, -1 otherwise
static int resolve_index(const hes_devreg_t* reg, const char* path, uint32_t di)
{
    int idx = index_by_di(reg, di);
    if (idx >= 0 && reg->devices[idx].online && hes_dev_object_find(&reg->devices[idx], path)) {
        return idx;
    }
    if (di != 0) {
        return -1;
    }
    // legacy: no device_index -> unambiguous single online device for path
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
/// Button value: prefer the live BLE backend value (bluez GATT notify)
/// once the manager has one; else fall back to the simulated tag.
///
/// @param ble The BLE manager context
/// @param tag The simulated SensorTag context
/// @param di The deviceIndex of the message
/// @param pressed Output parameter for the button state (1 pressed / 0 released)
/// @return 0 on success, non-zero on failure
static int read_button_any(const hes_ble_mgr_t* ble,
                           const sensortag_t* tag,
                           uint32_t di,
                           int* pressed)
{
    if (hes_ble_mgr_button(ble, di, pressed) == 0) {
        return 0;
    }
    return sensortag_read_button((sensortag_t*)tag, pressed);
}

////////////////////////////////////////////////////////////////////////////////
/// Reads one object's current value into buf (stringified). 0 on success.
///
/// @param ble The BLE manager context
/// @param tag The simulated SensorTag context
/// @param di The deviceIndex of the message
/// @param obj The device object to read
/// @param buf Output buffer for the stringified value
/// @param bufsz Size of the output buffer
/// @return 0 on success, non-zero on failure
static int read_object(const hes_ble_mgr_t* ble,
                       const sensortag_t* tag,
                       uint32_t di,
                       const hes_dev_object_t* obj,
                       char* buf,
                       size_t bufsz)
{
    if (strcmp(obj->path, HES_LX_BTN_CV) == 0) {
        int pressed;
        if (read_button_any(ble, tag, di, &pressed) != 0) {
            return -1;
        }
        snprintf(buf, bufsz, "%d", pressed ? 1 : 0);
        return 0;
    }
    if (strcmp(obj->path, HES_LX_TEMP_CV) == 0) {
        double c;
        if (sensortag_read_temp_c((sensortag_t*)tag, &c) != 0) {
            return -1;
        }
        snprintf(buf, bufsz, "%.2f", c);
        return 0;
    }
    return -1;  // no handler for this object path yet
}

////////////////////////////////////////////////////////////////////////////////
/// Publishes one object's current value on the bus as an event-report.
///
/// @param bus The bus context
/// @param reg The device registry
/// @param idx The index of the device in reg.devices[]
/// @param obj The device object to publish
/// @param val The stringified value to publish
static void publish_value(hes_bus_t* bus,
                          const hes_devreg_t* reg,
                          int idx,
                          const hes_dev_object_t* obj,
                          const char* val)
{
    hes_clme_msg_t msg = {0};
    msg.verb = HES_VERB_EVENT;
    msg.device_index = reg->devices[idx].device_index;
    hes_msg_set_path(&msg, obj->path);
    strncpy(msg.query, "va", sizeof(msg.query) - 1);
    hes_msg_set_payload_str(&msg, val);
    hes_bus_send(bus, &msg);
    log_info("ble: EVENT %s di=%u = %s", obj->path, msg.device_index, val);
}

////////////////////////////////////////////////////////////////////////////////
/// Merges a freshly received slice into our live registry (match by di).
///
/// @param reg The device registry to update
/// @param upd The updated device registry
static void merge_slice(hes_devreg_t* reg, const hes_devreg_t* upd)
{
    for (int i = 0; i < reg->n_devices; i++) {
        int j = index_by_di(upd, reg->devices[i].device_index);
        if (j < 0) {
            continue;
        }
        if (reg->devices[i].online != upd->devices[j].online) {
            log_info("ble: device di=%u now %s", reg->devices[i].device_index,
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

int han_ble_main(const char* svc_rep,
                 const char* svc_pub,
                 uint32_t module_ref,
                 const char* hub_pub_url,
                 const char* hub_sub_url,
                 const char* mac_override)
{
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    log_info("device manifest service at rep=%s pub=%s (moduleRefIndex=%u)", svc_rep, svc_pub,
             module_ref);

    hes_mreg_t mreg = {0};
    // HES_MT_INTERFACE is this module's 'mt': (moduleType, moduleRefIndex) is the
    // identity the service matches a slice request against (see hes_mreg.h).
    int rv = hes_mreg_open(&mreg, svc_rep, svc_pub, HES_MT_INTERFACE, module_ref);
    if (rv != 0) {
        log_error("cannot reach device manifest service");
        return 1;
    }

    hes_devreg_t reg = {0};
    rv = hes_mreg_query(&mreg, &reg);
    if (rv != 0) {
        log_error("no device slice from device manifest service");
        return 1;
    }

    // 0 assigned devices = profile / moduleRefIndex config error, NOT a
    // connectivity state (a powered-off tag only clears `online`). Fail fast
    // before the pointless report round-trip. See README "Why? FAQ".
    if (reg.n_devices == 0) {
        log_error(
                "no devices assigned to moduleRefIndex %u by the product "
                "profile; check product_profile.json and module_ref (config "
                "error, not a connection problem)",
                module_ref);
        return 1;
    }

    // Give every assigned device a BLE MAC (see README "Why a fabricated
    // MAC"): mac_override (the real MAC) wins; else fabricate a deterministic
    // 24:71:89:BC:32:<ni&0xff> pseudo-MAC. `online` is NOT hardcoded here --
    // the BLE connection manager below owns presence.
    for (int i = 0; i < reg.n_devices; i++) {
        if (mac_override && reg.devices[i].address[0] == '\0') {
            snprintf(reg.devices[i].address, sizeof(reg.devices[i].address), "%s", mac_override);
        } else if (reg.devices[i].address[0] == '\0') {
            snprintf(reg.devices[i].address, sizeof(reg.devices[i].address), "24:71:89:BC:32:%02u",
                     reg.devices[i].net_ref_index & 0xff);
        }

        reg.devices[i].online = 0;  // offline until the BLE link is up
    }

    // Boot-time registration handshake (see README): announce we manage these
    // devices (unbound -> bound), publish our addresses as runtime_address, and
    // seed presence as offline-until-link-up.
    rv = hes_mreg_report(&mreg, &reg);
    if (rv != 0) {
        log_warn("warning: report failed");
    }

    // Re-query: adopt the service's normalized view of our report (bound/offline
    // status + the stored addresses) before we start serving.
    hes_devreg_t reg2 = {0};
    rv = hes_mreg_query(&mreg, &reg2);
    if (rv != 0) {
        log_error("no device slice from device manifest service after report");
        return 1;
    }
    reg = reg2;

    // BLE connection manager (backend-agnostic, see hes_ble.h). Backend chosen
    // at BUILD time by CMake BLE_BACKEND: bluez (default, real) or sim -- no
    // runtime switch (see README "Backends"). Emits link-up/link-loss events.
    hes_ble_mgr_t ble = {0};
    hes_ble_mgr_init(&ble);
    for (int i = 0; i < reg.n_devices; i++) {
        hes_ble_mgr_add(&ble, reg.devices[i].device_index, reg.devices[i].address);
    }

    // Per-device sensor contexts (devs[] parallel to reg.devices[]): simulated
    // readings + button change tracking (btn_set/last_btn). See README.
    ble_dev_t devs[HES_DEVREG_MAX_DEVICES] = {0};
    for (int i = 0; i < reg.n_devices; i++) {
        sensortag_init(&devs[i].tag);

        // SIM-only: arm the simulated button auto-toggle (staggered per device)
        // so the sim demo emits press/release with no hardware. Compiled out in
        // a real bluez build, where the button comes only from GATT notify.
#ifdef HES_BLE_BACKEND_SIM
        for (int k = 0; k < reg.devices[i].n_objects; k++) {
            if (strcmp(reg.devices[i].objects[k].path, HES_LX_BTN_CV) == 0) {
                int period = 6 + (int)(reg.devices[i].device_index % 5) * 3;
                sensortag_set_button_period(&devs[i].tag, period);
                break;
            }
        }
#endif

        log_info("device di=%u ni=%u (%s) %s (%d object(s))", reg.devices[i].device_index,
                 reg.devices[i].net_ref_index, reg.devices[i].role, reg.devices[i].address,
                 reg.devices[i].n_objects);
    }

    // Owned object paths (deduped union across devices). hes_bus_leaf_open()
    // SUBscribes to exactly these, so we only ever receive traffic for objects
    // we serve (A.1 transport filtering). topicbuf = storage; topics[] = ptrs.
    char topicbuf[HES_BUS_MAX_TOPICS][HES_PATH_MAX] = {0};
    const char* topics[HES_BUS_MAX_TOPICS] = {0};
    int n_topics = hes_devreg_collect_topics(&reg, topicbuf, HES_BUS_MAX_TOPICS);
    for (int i = 0; i < n_topics; i++) {
        topics[i] = topicbuf[i];
    }

    log_info("dialing core module (sub<-%s, pub->%s)\n", hub_pub_url, hub_sub_url);
    hes_bus_t bus = {0};
    rv = hes_bus_leaf_open(&bus, hub_pub_url, hub_sub_url, topics, n_topics);
    if (rv != 0) {
        log_error("cannot connect to core module");
        return 1;
    }

    log_info("connected; %d device(s), %d topic(s). Publishing every %ds. Ctrl-C to stop.",
             reg.n_devices, n_topics, PUBLISH_PERIOD_SEC);

    // Main event loop -- five tasks per pass (details in README):
    //   1. recv + handle one downstream msg (recv blocks up to
    //      BUS_RECV_TIMEOUT_MS when idle -- that cadence also bounds how fast
    //      the bluez button notify is drained in step 3, so it sets the
    //      button's latency; see the define above)
    //   2. apply Device-Manifest notification
    //   3. drive BLE manager -> report presence changes
    //   4. button: publish on change (immediate)
    //   5. periodic report of slow/continuous sensors (temperature, 5 s)
    // Steps 4 and 5 are complementary: discrete events go out instantly,
    // continuous values on a timer.
    time_t last_publish = 0;
    while (g_running) {
        hes_clme_msg_t in = {0};
        int rv = hes_bus_recv(&bus, &in, BUS_RECV_TIMEOUT_MS);
        if (rv == 0) {
            if (in.verb == HES_VERB_SUBSCRIBE) {
                // SUBSCRIBE from the binding map: log, per ONLINE device owning
                // in.path, that we will stream it. No state kept -- we already
                // publish periodically / on change. See README.
                for (int i = 0; i < reg.n_devices; i++) {
                    if (reg.devices[i].online && hes_dev_object_find(&reg.devices[i], in.path)) {
                        log_info("subscription confirmed for %s (di=%u)", in.path,
                                 reg.devices[i].device_index);
                    }
                }
            } else if (in.verb == HES_VERB_GET) {
                int idx = resolve_index(&reg, in.path, in.device_index);
                const hes_dev_object_t* obj =
                        (idx >= 0) ? hes_dev_object_find(&reg.devices[idx], in.path) : NULL;
                if (idx >= 0 && obj && strchr(obj->access, 'r')) {
                    char val[32];
                    if (read_object(&ble, &devs[idx].tag, reg.devices[idx].device_index, obj, val,
                                    sizeof(val)) == 0) {
                        hes_clme_msg_t out = {0};
                        out.verb = HES_VERB_EVENT;
                        out.device_index = reg.devices[idx].device_index;
                        hes_msg_set_path(&out, in.path);
                        hes_msg_set_payload_str(&out, val);
                        hes_bus_send(&bus, &out);
                    }
                }
            }
            // PUTs are ignored: a SensorTag is a read-only (sensor) node.
        }

        // Apply any manifest-service notification (device went offline etc).
        hes_devreg_t upd;
        if (hes_mreg_recv_update(&mreg, &upd, 0) == 0) {
            log_info("ble: manifest notification received");
            merge_slice(&reg, &upd);
        }

        // Drive the BLE connection state machines. On link up/down, update our
        // device's online flag and report the presence change to the manifest
        // service (which notifies subscribers).
        hes_ble_event_t ev[HES_DEVREG_MAX_DEVICES];
        int nev = hes_ble_mgr_poll(&ble, ev, HES_DEVREG_MAX_DEVICES);
        for (int k = 0; k < nev; k++) {
            int idx = index_by_di(&reg, ev[k].device_index);
            if (idx < 0) {
                continue;
            }
            if (ev[k].is_online && !reg.devices[idx].online) {
                reg.devices[idx].online = 1;
                devs[idx].btn_set = 0;  // resync button on the new link
                log_info("ble: BLE link up di=%u, reporting online", ev[k].device_index);
                hes_mreg_report(&mreg, &reg);
            } else if (!ev[k].is_online && reg.devices[idx].online) {
                reg.devices[idx].online = 0;
                log_info("ble: BLE link lost di=%u, reporting offline", ev[k].device_index);
                hes_mreg_report(&mreg, &reg);
            }
        }

        // Section 4: change-driven button publish -- immediate (real GATT
        // notify via bluez, or sim auto-toggle). See README.
        for (int i = 0; i < reg.n_devices; i++) {
            if (!reg.devices[i].online) {
                continue;
            }

            const hes_dev_object_t* bo = hes_dev_object_find(&reg.devices[i], HES_LX_BTN_CV);
            if (!bo) {
                continue;
            }

            int pressed;
            if (read_button_any(&ble, &devs[i].tag, reg.devices[i].device_index, &pressed) != 0) {
                continue;
            }

            ble_dev_t* bd = &devs[i];
            if (!bd->btn_set || pressed != bd->last_btn) {
                bd->btn_set = 1;
                bd->last_btn = pressed;
                char val[8];
                snprintf(val, sizeof(val), "%d", pressed ? 1 : 0);
                publish_value(&bus, &reg, i, bo, val);
            }
        }

        // Section 5: periodic publish of the slow/continuous sensors (e.g.
        // temperature). The button is skipped here -- it is change-driven in
        // Section 4 and must never be delayed by this timer. See README.
        time_t now = time(NULL);
        if (now - last_publish >= PUBLISH_PERIOD_SEC) {
            last_publish = now;
            for (int i = 0; i < reg.n_devices; i++) {
                if (!reg.devices[i].online) {
                    continue;
                }

                for (int k = 0; k < reg.devices[i].n_objects; k++) {
                    const hes_dev_object_t* obj = &reg.devices[i].objects[k];
                    if (!strchr(obj->access, 'r')) {
                        continue;
                    }

                    if (strcmp(obj->path, HES_LX_BTN_CV) == 0) {
                        continue;  // button handled in Section 4, not here
                    }

                    char buf[32];
                    if (read_object(&ble, &devs[i].tag, reg.devices[i].device_index, obj, buf,
                                    sizeof(buf)) == 0) {
                        publish_value(&bus, &reg, i, obj, buf);
                    }
                }
            }
        }
    }

    log_info("shutting down ble han interface module");
    hes_ble_mgr_close(&ble);
    hes_mreg_close(&mreg);
    hes_bus_close(&bus);
    return 0;
}
