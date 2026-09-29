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
/// @brief Ambient-light accessor for the SensorTag CC2650's OPT3001, mirroring
/// tests/sensortag_light_test.c.
///
/// @details
/// Real protocol: GATT data characteristic f000aa71-0451-4000-b000-000000000000,
/// config f000aa72-0451-4000-b000-000000000000 (enable = write 0x01).
/// The raw payload is 2 bytes read as a uint16 little-endian OPT3001
/// "modified float":
///   mantissa = raw & 0x0FFF
///   exponent = (raw >> 12) & 0x0F
///   lux      = mantissa * 0.01 * (1 << exponent)
///
/// Simulated here (no BlueZ in the sandbox -- see sensortag.h): a slow
/// random walk around indoor illuminance. Swap the body for a real
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
static double last_lux = 300.0;

int sensortag_read_light_lux(double* out_lux)
{
    if (!seeded) {
        srand((unsigned)time(NULL));
        seeded = 1;
    }
    // +/- 40 lux per step -- indoor ambient light with the odd swing.
    double delta = ((rand() % 161) - 80) / 2.0;
    last_lux += delta;
    if (last_lux < 0.0) {
        last_lux = 0.0;
    }
    if (last_lux > 900.0) {
        last_lux = 900.0;
    }

    *out_lux = last_lux;
    return 0;
}
