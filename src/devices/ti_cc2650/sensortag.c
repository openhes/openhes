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
/// @file
/// @brief The SensorTag device context and its ambient-temperature reading
/// (see sensortag.h).
///
/// @details
/// This unit owns the per-device context (sensortag_init) and, with it, the
/// simulated ambient-temperature read plus the simulated button state and its
/// auto-toggle cadence, which the BLE module polls.
///
/// The temperature read is a slow random walk, clamped to a plausible indoor
/// range and seeded per context -- so several SensorTags in one process follow
/// independent walks, the way separate GATT connections would.
///
/// Memory/ownership: no heap. The context belongs to the caller and this unit
/// keeps no state of its own.
///
/// Threading: no locking; a context belongs to one thread.
#include "sensortag.h"
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

void sensortag_init(sensortag_t* st)
{
    st->seeded = 0;
    st->last_temp_c = 22.0;  // room temperature, deg C
    st->button_state = 0;
    st->button_period_s = 0;
    st->next_button_flip = 0;
}

int sensortag_read_temp_c(sensortag_t* st, double* out_temp_c)
{
    if (!st->seeded) {
        // Seed per device context so two SensorTags in one process follow
        // independent walks (with real hardware this would be per-GATT
        // connection state).
        srand((unsigned)(time(NULL) ^ (unsigned long)(uintptr_t)st));
        st->seeded = 1;
    }
    // Slow random walk, clamped to a plausible indoor range, standing
    // in for a real CC2650 IR-temperature-sensor GATT read (see the
    // header for why real BlueZ integration isn't wired in here).
    double delta = ((rand() % 21) - 10) / 20.0;  // +/- 0.5 deg C
    st->last_temp_c += delta;
    if (st->last_temp_c < 15.0) {
        st->last_temp_c = 15.0;
    }
    if (st->last_temp_c > 30.0) {
        st->last_temp_c = 30.0;
    }

    *out_temp_c = st->last_temp_c;
    return 0;
}

void sensortag_set_button_period(sensortag_t* st, int period_s)
{
    st->button_period_s = period_s;
    if (period_s <= 0) {
        st->next_button_flip = 0;
        return;
    }
    if (st->next_button_flip == 0) {
        st->next_button_flip = time(NULL) + (time_t)period_s;
    }
}

int sensortag_read_button(sensortag_t* st, int* out_pressed)
{
    time_t now = time(NULL);
    if (st->button_period_s > 0 && st->next_button_flip != 0 && now >= st->next_button_flip) {
        // Flip the switch and schedule the next toggle. Different devices
        // use different periods, so identical SensorTag buttons drift apart.
        st->button_state = !st->button_state;
        st->next_button_flip = now + (time_t)st->button_period_s;
    }
    *out_pressed = st->button_state;
    return 0;
}
