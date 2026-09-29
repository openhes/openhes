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
/// @brief Microphone-level accessor for the SensorTag CC2650's SPH0641,
/// mirroring tests/sensortag_mic_test.c.
///
/// @details
/// Real protocol: GATT data characteristic f000aa61-0451-4000-b000-000000000000,
/// config f000aa62-0451-4000-b000-000000000000 (enable = write 0x01).
/// The raw payload is a run of unsigned 8-bit audio samples; the module
/// reports the mean absolute deviation from the 8-bit zero baseline 128,
/// an uncalibrated relative amplitude (~0-128 typical, not dB SPL):
///   level = sum(|sample - 128|) / n
///
/// Simulated here (no BlueZ in the sandbox -- see sensortag.h): a slow
/// random walk around a quiet-room level. Swap the body for a real
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
static double last_level = 15.0;

int sensortag_read_mic_level(double* out_level)
{
    if (!seeded) {
        srand((unsigned)time(NULL));
        seeded = 1;
    }
    // +/- 8 per step -- quiet room with the odd louder sound.
    double delta = ((rand() % 161) - 80) / 10.0;
    last_level += delta;
    if (last_level < 0.0) {
        last_level = 0.0;
    }
    if (last_level > 128.0) {
        last_level = 128.0;
    }

    *out_level = last_level;
    return 0;
}
