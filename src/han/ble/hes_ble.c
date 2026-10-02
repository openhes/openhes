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
/// @brief BLE connection manager, SIM backend: a simulated radio that turns tag
/// power-on/off into link-up/link-loss events (see hes_ble.h).
///
/// @details
/// This is the default "sim" backend. It models each managed SensorTag as
/// a radio that is powered for `power_on_s` seconds, then off for
/// `power_off_s` seconds, repeating (a stand-in for you physically
/// switching the tag on/off during a demo). The connection manager turns
/// those power changes into link-up / link-loss events:
///
///   powered on  -> CONNECTING -> (after sim_connect_s) -> ONLINE
///   powered off -> link loss -> OFFLINE event (emitted immediately)
///
/// A real "bluez" backend would feed the same state machines from BlueZ
/// D-Bus signals instead (scan -> Connect -> ServicesResolved;
/// PropertiesChanged Connected=false; reconnect). Swap by replacing this
/// file's poll() body; the manager API and the module logic stay the
/// same.

#include "hes_ble.h"

#include <stdio.h>

#define HES_BLE_SIM_CONNECT_S 1    ///< ~1 s advertising->connected
#define HES_BLE_SIM_POWER_ON_S 20  ///< tag stays on 20 s
#define HES_BLE_SIM_POWER_OFF_S 8  ///< then off 8 s

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

void hes_ble_mgr_init(hes_ble_mgr_t* m)
{
    m->n = 0;
    m->sim_connect_s = HES_BLE_SIM_CONNECT_S;
    m->sim_power_on_s = HES_BLE_SIM_POWER_ON_S;
    m->sim_power_off_s = HES_BLE_SIM_POWER_OFF_S;
    for (int i = 0; i < HES_BLE_MAX_DEVS; i++) {
        m->dev[i].device_index = 0;
        m->dev[i].state = HES_BLE_STATE_OFF;
        m->dev[i].sim_powered = 0;
        m->dev[i].have_button = 0;
        m->dev[i].button_state = 0;
    }
}

int hes_ble_mgr_add(hes_ble_mgr_t* m, uint32_t device_index, const char* address)
{
    (void)address;  // SIM backend ignores the real MAC
    for (int i = 0; i < m->n; i++) {
        if (m->dev[i].device_index == device_index) {
            return i;  // already managed
        }
    }

    if (m->n >= HES_BLE_MAX_DEVS) {
        return -1;
    }

    hes_ble_conn_t* d = &m->dev[m->n++];
    d->device_index = device_index;
    // (sim) start powered-on and advertising: comes online in ~1 s
    d->sim_powered = 1;
    d->state = HES_BLE_STATE_CONNECTING;
    d->state_entered = time(NULL);
    d->next_power_flip = d->state_entered + m->sim_power_on_s;
    d->have_button = 0;
    d->button_state = 0;
    return m->n - 1;
}

int hes_ble_mgr_poll(hes_ble_mgr_t* m, hes_ble_event_t* events, int max)
{
    int nev = 0;
    time_t now = time(NULL);

    for (int i = 0; i < m->n; i++) {
        hes_ble_conn_t* d = &m->dev[i];

        // (sim) does the tag's power state change now?
        if (now >= d->next_power_flip) {
            d->sim_powered = !d->sim_powered;
            d->next_power_flip = now + (d->sim_powered ? m->sim_power_on_s : m->sim_power_off_s);

            if (d->sim_powered) {
                // powered back on: starts advertising -> connecting
                d->state = HES_BLE_STATE_CONNECTING;
                d->state_entered = now;
                fprintf(stderr, "hes_ble[sim]: di=%u powered ON, connecting...\n", d->device_index);
            } else {
                // powered off: link loss (immediate detection here)
                if (d->state == HES_BLE_STATE_ONLINE && nev < max) {
                    events[nev].device_index = d->device_index;
                    events[nev].is_online = 0;
                    nev++;
                }
                d->state = HES_BLE_STATE_OFF;
                d->state_entered = now;
                fprintf(stderr, "hes_ble[sim]: di=%u powered OFF (link lost)\n", d->device_index);
            }
        }

        // connecting -> online once the (simulated) connect completes
        if (d->sim_powered && d->state == HES_BLE_STATE_CONNECTING &&
            now - d->state_entered >= m->sim_connect_s) {
            d->state = HES_BLE_STATE_ONLINE;
            d->state_entered = now;
            if (nev < max) {
                events[nev].device_index = d->device_index;
                events[nev].is_online = 1;
                nev++;
            }
            fprintf(stderr, "hes_ble[sim]: di=%u connected\n", d->device_index);
        }
    }

    return nev;
}

void hes_ble_mgr_close(hes_ble_mgr_t* m)
{
    (void)m;  // SIM backend has nothing to tear down
}

int hes_ble_mgr_button(const hes_ble_mgr_t* m, uint32_t device_index, int* pressed)
{
    (void)m;
    (void)device_index;
    (void)pressed;
    // SIM backend has no live hardware button; the module uses its own
    // simulated reading.
    return -1;
}
