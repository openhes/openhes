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
/// @brief Relative-humidity accessor for the SensorTag CC2650's HDC1000,
/// mirroring tests/sensortag_humid_test.c.
///
/// @details
/// Real protocol: GATT data characteristic f000aa21-0451-4000-b000-000000000000,
/// config f000aa22-0451-4000-b000-000000000000 (enable = write 0x01).
/// The raw payload is 4 bytes: bytes 0..1 are the temperature and bytes
/// 2..3 the humidity, each a uint16 little-endian:
///   temp_c = raw_temp / 65536.0 * 165.0 - 40.0
///   rh_pct = raw_humid / 65536.0 * 100.0
/// (Temperature is covered by sensortag_read_temp_c, so only RH is
/// exposed here.)
///
/// Simulated here (no BlueZ in the sandbox -- see sensortag.h): a slow
/// random walk around a plausible indoor humidity. Swap the body for a
/// real BlueZ D-Bus GATT read when real hardware is available.

#include <stdlib.h>
#include <time.h>
#include "sensortag.h"

static int seeded = 0;
static double last_rh = 45.0;  ///< % RH

int sensortag_read_humidity_pct(double* out_rh_pct)
{
    if (!seeded) {
        srand((unsigned)time(NULL));
        seeded = 1;
    }
    // +/- 1.5 % RH per step, clamped to a plausible indoor band.
    double delta = ((rand() % 301) - 150) / 100.0;
    last_rh += delta;
    if (last_rh < 20.0) {
        last_rh = 20.0;
    }
    if (last_rh > 80.0) {
        last_rh = 80.0;
    }

    *out_rh_pct = last_rh;
    return 0;
}
