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
/// @brief Accessors for a TI SensorTag CC2650's sensors and inputs, per the
/// product profile manifest's expectedHardware.
///
/// @details
/// This is the device layer's library-style API: plain function calls that
/// turn a SensorTag connection into a sensor reading, with no HES, no event
/// bus and no manifest involvement (see ../README.md). Each accessor mirrors
/// one of the standalone TI SensorTag readers in tests/
/// (sensortag_bar_test.c, sensortag_battery_test.c, sensortag_button_test.c,
/// sensortag_humid_test.c, sensortag_light_test.c, sensortag_mic_test.c,
/// sensortag_motion_test.c), packaged as a plain function call.
///
/// IMPORTANT / KNOWN LIMITATION:
///   A real GATT connection to a CC2650 SensorTag goes through BlueZ 5's
///   D-Bus API (org.bluez.Device1 / org.bluez.GattCharacteristic1 -- a
///   sensor is enabled by writing to its "Config" characteristic, then
///   its "Data" characteristic is read or subscribed and the raw bytes
///   are decoded with TI's documented conversion formulas). That needs
///   BlueZ, a paired/trusted device, and a real adapter -- none of which
///   exist in a text/sandbox environment, so it can't be written and
///   verified responsibly here.
///
///   Every accessor is therefore a SIMULATED reading (a slow random walk
///   around a plausible value) so the rest of the pipeline -- bus,
///   binding map, WiFi module -- stays fully runnable and testable
///   end-to-end today. Each declaration below notes the real GATT
///   characteristic UUIDs, enable byte and conversion from its
///   tests/sensortag_*_test.c counterpart; swap each function body for a
///   real BlueZ D-Bus GATT read (e.g. GDBus/GLib, or a BLE library such as
///   SimpleBLE) when you have real hardware and an adapter to test against --
///   nothing else in the calling interface module needs to change.

#ifndef OPENHES_SRC_DEVICES_TI_CC2650_SENSORTAG_H
#define OPENHES_SRC_DEVICES_TI_CC2650_SENSORTAG_H

#include <stdint.h>
#include <time.h>

////////////////////////////////////////////////////////////////////////////////
/// One SensorTag device context. The BLE module hosts one context per
/// SensorTag in its Device-Manifest slice, so several identical SensorTags
/// each keep an independent simulated state (and, with real hardware, an
/// independent GATT connection).
///
/// Each SensorTag here is simulated; a given device may expose a
/// temperature sensor, a button/switch, or both. Button state can be
/// toggled automatically on a per-device cadence so the demo generates
/// events without real hardware.
typedef struct sensortag {
    int seeded;
    double last_temp_c;       ///< room temperature, deg C
    int button_state;         ///< simulated button/switch: 1 pressed, 0 released
    int button_period_s;      ///< auto-toggle period; 0 = hold current state
    time_t next_button_flip;  ///< next scheduled toggle (0 = none)
} sensortag_t;

void sensortag_init(sensortag_t* st);

////////////////////////////////////////////////////////////////////////////////
/// Ambient temperature, degrees C. The real read is the IR thermopile
/// (data f000aa00-..., config f000aa01-..., enable 0x01), two little-endian
/// int16 raw values decoded per TI's formula. Simulated here as a slow
/// random walk per device context. Returns 0 and fills *out_temp_c.
int sensortag_read_temp_c(sensortag_t* st, double* out_temp_c);

////////////////////////////////////////////////////////////////////////////////
/// Configure the simulated button to toggle automatically every period_s
/// seconds (0 disables auto-toggle and holds the current state).
void sensortag_set_button_period(sensortag_t* st, int period_s);

////////////////////////////////////////////////////////////////////////////////
/// Read the (simulated) button/switch state: 1 pressed/closed,
/// 0 released/open. Returns 0 and fills *out_pressed.
int sensortag_read_button(sensortag_t* st, int* out_pressed);

////////////////////////////////////////////////////////////////////////////////
/// Barometric pressure, hPa. Real read (tests/sensortag_bar_test.c):
/// BMP280, data f000aa41-..., config f000aa42-... enable 0x01; raw
/// payload >= 6 bytes, bytes 3..5 are a 24-bit little-endian unsigned
/// integer of hPa*100. Returns 0 and fills *out_hpa on success.
int sensortag_read_pressure_hpa(double* out_hpa);

////////////////////////////////////////////////////////////////////////////////
/// Battery charge, percent. Real read (tests/sensortag_battery_test.c):
/// standard battery characteristic 00002a19-0000-1000-8000-00805f9b34fb,
/// one raw byte, no config write. Returns 0 and fills *out_pct.
int sensortag_read_battery_pct(int* out_pct);

////////////////////////////////////////////////////////////////////////////////
/// Relative humidity, percent RH. Real read (tests/sensortag_humid_test.c):
/// HDC1000, data f000aa21-..., config f000aa22-... enable 0x01; 4 raw
/// bytes = temperature then RH as uint16 little-endian, RH = raw/65536*100.
/// (That sensor also reports its own temperature, but this module uses
/// sensortag_read_temp_c for temperature.) Returns 0 and fills *out_rh.
int sensortag_read_humidity_pct(double* out_rh_pct);

////////////////////////////////////////////////////////////////////////////////
/// Ambient light, lux. Real read (tests/sensortag_light_test.c): OPT3001,
/// data f000aa71-..., config f000aa72-... enable 0x01; 2 raw bytes as a
/// uint16 little-endian OPT3001 float (lux = mantissa*0.01*2^exponent).
/// Returns 0 and fills *out_lux.
int sensortag_read_light_lux(double* out_lux);

////////////////////////////////////////////////////////////////////////////////
/// Microphone level, relative amplitude (mean |sample - 128| over the
/// 8-bit samples; ~0-128, not calibrated dB). Real read
/// (tests/sensortag_mic_test.c): SPH0641, data f000aa61-..., config
/// f000aa62-... enable 0x01. Returns 0 and fills *out_level.
int sensortag_read_mic_level(double* out_level);

/// Button / reed-switch inputs, as a bitmask of the state byte:
///   0x01 LEFT button pressed, 0x02 RIGHT button pressed, 0x04 REED
/// switch (magnet) present. Real read (tests/sensortag_button_test.c) is
/// notify-based on a fixed characteristic path; this simulated accessor
/// returns a snapshot of the current bits. Returns 0 and fills *out_bits.
#define SENSORTAG_BTN_LEFT 0x01
#define SENSORTAG_BTN_RIGHT 0x02
#define SENSORTAG_BTN_REED 0x04
int sensortag_read_button_bits(uint8_t* out_bits);

////////////////////////////////////////////////////////////////////////////////
/// A single 9-axis IMU sample.
typedef struct sensortag_motion_sample {
    double accel_g[3];   ///< X, Y, Z -- acceleration, g
    double gyro_dps[3];  ///< X, Y, Z -- angular rate, deg/s
    double mag_ut[3];    ///< X, Y, Z -- magnetic field, microtesla
} sensortag_motion_sample;

////////////////////////////////////////////////////////////////////////////////
/// Motion sample. Real read (tests/sensortag_motion_test.c): MPU-9250,
/// data f000aa81-..., config f000aa82-... enable {0x7F, 0x00}; 18 raw
/// bytes = 9 x int16 little-endian (gyro XYZ, accel XYZ, mag XYZ).
/// Returns 0 and fills *out on success.
int sensortag_read_motion(sensortag_motion_sample* out);

#endif  // #ifndef OPENHES_SRC_DEVICES_TI_CC2650_SENSORTAG_H
