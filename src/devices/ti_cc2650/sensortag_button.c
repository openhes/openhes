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
/// @brief Button / reed-switch input accessor for the SensorTag CC2650,
/// mirroring tests/sensortag_button_test.c.
///
/// @details
/// Real protocol: the SensorTag's key service is notify-based (StartNotify
/// on a fixed characteristic path, then one byte per event), so it is
/// event-driven rather than readable on demand. Bit layout of the state
/// byte:
///   0x01 LEFT button pressed
///   0x02 RIGHT button pressed
///   0x04 REED switch (magnet) present
///
/// Simulated here (no BlueZ in the sandbox -- see sensortag.h): returns a
/// snapshot of the current bits, with an occasional simulated press.
/// Swap the body for a real BlueZ D-Bus notification handler.
///
/// Memory/ownership: writes the bit snapshot into the caller's out-parameter; no
/// allocation.
///
/// Threading: not reentrant -- the simulated press sequence lives in file-static
/// state and the accessor takes no device context (see the note in sensortag.h).
#include <stdlib.h>
#include <time.h>
#include "sensortag.h"

static int seeded = 0;
static int press_seq = 0;

int sensortag_read_button_bits(uint8_t* out_bits)
{
    if (!seeded) {
        srand((unsigned)time(NULL));
        seeded = 1;
    }
    uint8_t bits = 0;
    // ~1 poll in 10 simulates a short press, cycling LEFT/RIGHT/REED.
    if (rand() % 10 == 0) {
        bits = (uint8_t)(1u << (press_seq % 3));
        press_seq++;
    }

    *out_bits = bits;
    return 0;
}
