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
/// @brief Barometric pressure accessor for the SensorTag CC2650's BMP280,
/// mirroring tests/sensortag_bar_test.c.
///
/// @details
/// Real protocol: GATT data characteristic f000aa41-0451-4000-b000-000000000000,
/// config f000aa42-0451-4000-b000-000000000000 (enable = write 0x01,
/// disable = write 0x00). The raw payload is >= 6 bytes; bytes 3..5 are
/// a 24-bit little-endian unsigned integer in units of 1/100 hPa, so
/// dividing by 100.0 yields hPa.
///
/// Simulated here (no BlueZ in the sandbox -- see sensortag.h): a slow
/// random walk around sea-level pressure. Swap the body for a real
/// BlueZ D-Bus GATT read when real hardware is available.
///
/// Memory/ownership: writes the reading into the caller's out-parameter; no
/// allocation and no state of its own.
///
/// Threading: no locking -- the caller drives it from one loop.
#include <stdlib.h>
#include <time.h>
#include "sensortag.h"

static int seeded = 0;
static double last_pressure_hpa = 1013.0;  ///< mean sea level, hPa

int sensortag_read_pressure_hpa(double* out_hpa)
{
    if (!seeded) {
        srand((unsigned)time(NULL));
        seeded = 1;
    }
    // +/- 1.0 hPa per step, clamped to a plausible weather band.
    double delta = ((rand() % 201) - 100) / 100.0;
    last_pressure_hpa += delta;
    if (last_pressure_hpa < 985.0) {
        last_pressure_hpa = 985.0;
    }
    if (last_pressure_hpa > 1035.0) {
        last_pressure_hpa = 1035.0;
    }

    *out_hpa = last_pressure_hpa;
    return 0;
}
