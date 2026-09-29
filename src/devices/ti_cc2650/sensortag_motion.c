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
/// @brief 9-axis IMU accessor for the SensorTag CC2650's MPU-9250, mirroring
/// tests/sensortag_motion_test.c.
///
/// @details
/// Real protocol: GATT data characteristic f000aa81-0451-4000-b000-000000000000,
/// config f000aa82-0451-4000-b000-000000000000 (enable = write the two
/// bytes {0x7F, 0x00} for all nine axes; disable = {0x00, 0x00}). The raw
/// payload is 18 bytes = 9 x int16 little-endian: gyro XYZ at bytes 0..5,
/// accel XYZ at bytes 6..11, mag XYZ at bytes 12..17. Scaling, kept
/// verbatim from tests/sensortag_motion_test.c:
///   gyro[i]  = raw[i]   * (500.0 / 65536.0)  -- deg/s
///   accel[i] = raw[i+3] * (  8.0 / 32768.0)  -- g
///   mag[i]   = raw[i+6] * 1.0                -- uT
///
/// Simulated here (no BlueZ in the sandbox -- see sensortag.h): a
/// SensorTag resting on a table (+1 g down Z, gyros idle, ~41 uT field
/// split mostly X/Z) with small noise on every axis. Swap the body for a
/// real BlueZ D-Bus GATT read when real hardware is available.
///
/// Memory/ownership: writes the sample into the caller's out-parameter; no
/// allocation and no state of its own.
///
/// Threading: no locking -- the caller drives it from one loop.
#include <stdlib.h>
#include <time.h>
#include "sensortag.h"

static int seeded = 0;

int sensortag_read_motion(sensortag_motion_sample* out)
{
    if (!seeded) {
        srand((unsigned)time(NULL));
        seeded = 1;
    }
    const double acc_nom[3] = {0.0, 0.0, 1.0};
    const double gyro_nom[3] = {0.0, 0.0, 0.0};
    const double mag_nom[3] = {25.0, 0.0, 35.0};

    for (int i = 0; i < 3; i++) {
        out->accel_g[i] = acc_nom[i] + ((rand() % 101) - 50) / 2500.0;  // +/-0.02 g
        out->gyro_dps[i] = gyro_nom[i] + ((rand() % 101) - 50) / 50.0;  // +/-1 deg/s
        out->mag_ut[i] = mag_nom[i] + ((rand() % 101) - 50) / 100.0;    // +/-0.5 uT
    }
    return 0;
}
