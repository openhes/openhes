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
/// @brief Battery charge accessor for the SensorTag CC2650's CR2032 cell,
/// mirroring tests/sensortag_battery_test.c.
///
/// @details
/// Real protocol: standard GATT Battery Service, characteristic
/// 00002a19-0000-1000-8000-00805f9b34fb. One raw byte giving the charge
/// as a percentage; no config write is needed.
///
/// Simulated here (no BlueZ in the sandbox -- see sensortag.h): a slow
/// random walk around a healthy charge level. Swap the body for a real
/// BlueZ D-Bus GATT read when real hardware is available.

#include <stdlib.h>
#include <time.h>
#include "sensortag.h"

static int seeded = 0;
static double last_pct = 90.0;

int sensortag_read_battery_pct(int* out_pct)
{
    if (!seeded) {
        srand((unsigned)time(NULL));
        seeded = 1;
    }
    // +/- 0.2 % per step -- a battery barely moves between polls.
    double delta = ((rand() % 41) - 20) / 100.0;
    last_pct += delta;
    if (last_pct < 60.0) {
        last_pct = 60.0;
    }
    if (last_pct > 100.0) {
        last_pct = 100.0;
    }

    *out_pct = (int)(last_pct + 0.5);
    return 0;
}
