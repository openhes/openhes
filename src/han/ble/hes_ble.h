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
/// @brief BLE connection manager (backend-agnostic): one per-device presence
/// state machine, used by the BLE HAN interface module.
///
/// @details
/// Goal behaviour (see docs/poc_design.md):
///   - gateway powers on, SensorTag powers on  -> connects quickly;
///   - SensorTag powers off                    -> gateway senses offline
///     (link loss) and reports it (manifest service);
///   - SensorTag powers back on                 -> connection is
///     re-established quickly.
///
/// The manager owns one per-device state machine and is backend-agnostic:
/// it only *emits transitions* (device went online / went offline). What
/// actually produces those transitions is a backend, selected at build
/// time with BLE_BACKEND=sim|bluez:
///   - "sim"   (hes_ble.c, default): a simulated radio that models
///     the tag powering on/off on a schedule, so the manager + reconnect
///     policy are fully testable with no hardware;
///   - "bluez" (hes_ble_bluez.c): real BlueZ D-Bus via GLib/GDBus
///     (scan -> connect/pair -> services resolved; PropertiesChanged
///     link loss; reconnect).
///
/// The module calls hes_ble_mgr_poll() each loop iteration and reacts to
/// each event by updating its device's online flag and reporting to the
/// Device Manifest service.
///
/// Memory/ownership: the manager is a flat value structure in the caller's own
/// storage -- hes_ble_mgr_init() zeroes it, and whatever a backend needs lives
/// inside it. No allocation, no destructor.
///
/// Threading: no locking. hes_ble_mgr_poll() is called from the module's own
/// loop; the bluez backend's D-Bus work is pumped from that same call.
#ifndef OPENHES_SRC_HAN_BLE_HES_BLE_H
#define OPENHES_SRC_HAN_BLE_HES_BLE_H

#include <stdint.h>
#include <time.h>

#define HES_BLE_MAX_DEVS 16

typedef enum hes_ble_state {
    HES_BLE_STATE_OFF,         ///< tag not powered / no link
    HES_BLE_STATE_CONNECTING,  ///< advertising seen, connecting
    HES_BLE_STATE_ONLINE,      ///< connected, services resolved
} hes_ble_state_t;

////////////////////////////////////////////////////////////////////////////////
/// One state transition detected by the manager on a poll.
typedef struct hes_ble_event {
    uint32_t device_index;
    int is_online;  ///< 1 = link up, 0 = link lost
} hes_ble_event_t;

////////////////////////////////////////////////////////////////////////////////
/// Backend-agnostic per-device state. Backends may keep extra private
/// state alongside (keyed by device_index) in their own module.
typedef struct hes_ble_conn {
    uint32_t device_index;
    char address[64];  ///< BLE MAC; real bindings for the bluez backend
    hes_ble_state_t state;
    time_t state_entered;  ///< when the current state began

    /// SIM backend state: is the (simulated) tag powered on?
    int sim_powered;
    time_t next_power_flip;  ///< when the sim tag next powers on/off

    /// Live button value from the backend. When have_button is set the
    /// module prefers this over its own simulated reading (the bluez
    /// backend fills it from GATT notifications; sim never sets it).
    int have_button;
    int button_state;  ///< 1 pressed, 0 released (any SensorTag key)
} hes_ble_conn_t;

typedef struct hes_ble_mgr {
    int n;
    hes_ble_conn_t dev[HES_BLE_MAX_DEVS];

    /// sim timing knobs (seconds)
    int sim_connect_s;    ///< advertising seen -> connected
    int sim_power_on_s;   ///< how long the (sim) tag stays on
    int sim_power_off_s;  ///< how long it stays off
} hes_ble_mgr_t;

void hes_ble_mgr_init(hes_ble_mgr_t* m);

////////////////////////////////////////////////////////////////////////////////
/// Start managing a device with the given BLE address (MAC). The device
/// starts "powered on and connecting". Returns index or -1.
int hes_ble_mgr_add(hes_ble_mgr_t* m, uint32_t device_index, const char* address);

////////////////////////////////////////////////////////////////////////////////
/// Advance the state machines; writes transitions (since the last poll)
/// into events[] (max entries). Returns the number of events. Call on
/// every module loop iteration.
int hes_ble_mgr_poll(hes_ble_mgr_t* m, hes_ble_event_t* events, int max);

////////////////////////////////////////////////////////////////////////////////
/// Tear down (disconnect devices, free backend resources).
void hes_ble_mgr_close(hes_ble_mgr_t* m);

////////////////////////////////////////////////////////////////////////////////
/// If the backend has a live button value for device_index, return 0 and
/// set *pressed (1 pressed, 0 released); otherwise return -1 so the
/// module falls back to its own simulated reading.
int hes_ble_mgr_button(const hes_ble_mgr_t* m, uint32_t device_index, int* pressed);

#endif  // #ifndef OPENHES_SRC_HAN_BLE_HES_BLE_H
