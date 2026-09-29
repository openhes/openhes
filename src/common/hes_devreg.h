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
/// @brief Device registry: an interface module's slice of the runtime Device
/// Manifest (see docs/poc_design.md §9.3-9.4).
///
/// @details
/// A HAN/WAN interface module is the ONLY component that needs real
/// device information (MAC / IP / port): it opens the actual network
/// connection. So each interface-module process loads a manifest slice
/// describing the devices IT fronts, and builds:
///
///     deviceIndex -> (netRefIndex, network address, user-object list)
///
/// Lookups are by deviceIndex. Because several devices can share one
/// user-object path (two identical bulbs both expose /lx/ob/uo/li/ll/da/cv),
/// a received message is attributed to a device by deviceIndex -- the
/// CLIP-style 'di' query adopted as CLDPE manufacturer-defined addressing
/// (see docs/poc_design.md §9.5). Messages whose deviceIndex is not in
/// this slice are ignored, which is what lets the same device type be
/// hosted by one module OR spread across several modules.
///
/// Memory/ownership: a hes_devreg_t is a flat value structure (fixed arrays), so
/// there is nothing to allocate or free -- hes_devreg_init() zeroes it and a
/// slice is copied into it field by field.
///
/// Threading: no shared state; a registry belongs to the module that holds it.
///
#ifndef OPENHES_SRC_COMMON_HES_DEVREG_H
#define OPENHES_SRC_COMMON_HES_DEVREG_H

#include "hes_common.h"

#include <jansson.h>

#define HES_DEVREG_MAX_DEVICES 16
#define HES_DEVREG_MAX_OBJECTS 16

////////////////////////////////////////////////////////////////////////////////
/// Access modes, mirroring the CLIP/18012 usage of an object:
/// "r" read (sensor), "w" write (actuator), "rw" both.
typedef struct hes_dev_object {
    char path[HES_PATH_MAX];  ///< lexicon object path, e.g. /lx/ob/uo/li/ll/da/cv
    char access[4];           ///< "r" | "w" | "rw"
} hes_dev_object_t;

////////////////////////////////////////////////////////////////////////////////
/// One physical device behind this interface module.
typedef struct hes_device {
    uint32_t device_index;       ///< 'di' -- gateway-wide, matches addressingTable
    uint32_t net_ref_index;      ///< 'ni' -- this module's own index for the device
    char role[32];               ///< deviceRole, e.g. "Primary_Lighting_Actuator"
    char expected_hardware[48];  ///< expectedHardware from the profile
    char network_type[16];       ///< "BLE" | "WiFi" | ...
    char address[64];            ///< BLE MAC or IP address ("" = not bound yet)
    int udp_port;                ///< network type that uses UDP (0 = n/a)
    int online;                  ///< runtime presence; from the manifest service
    int n_objects;               ///< how many entries objects[] holds
    hes_dev_object_t objects[HES_DEVREG_MAX_OBJECTS];  ///< the device's objects
} hes_device_t;

////////////////////////////////////////////////////////////////////////////////
/// The manifest slice for ONE interface module (its moduleRefIndex /
/// moduleType / networkType + the devices it fronts).
typedef struct hes_devreg {
    char gateway_id[64];                           ///< gatewayId from the slice
    char module_type[8];                           ///< 'hi' / 'wi'
    uint32_t module_ref_index;                     ///< 'mi'
    char network_type[16];                         ///< "BLE" | "WiFi" | ...
    int n_devices;                                 ///< how many entries devices[] holds
    hes_device_t devices[HES_DEVREG_MAX_DEVICES];  ///< the devices this module fronts
} hes_devreg_t;

////////////////////////////////////////////////////////////////////////////////
/// Zeroes a registry, i.e. "this module fronts no devices yet".
///
/// @param reg The registry to reset.
////////////////////////////////////////////////////////////////////////////////
/// Loads the module's slice from a JSON Device-Manifest file.
///
/// (Legacy helper -- device slices now normally arrive over NNG from the
/// Device Manifest service, in which case use hes_devreg_parse() on the
/// received JSON instead.)
///
/// @param reg The registry to fill in.
/// @param json_path Path to the slice file.
/// @return 0 on success, -1 on error (message printed to stderr).
int hes_devreg_load_file(hes_devreg_t* reg, const char* json_path);

////////////////////////////////////////////////////////////////////////////////
/// Populates *reg from an already-parsed JSON object (the "slice" schema --
/// see the sample file under interface_modules/).
///
/// @param reg The registry to fill in.
/// @param root The parsed slice document.
/// @return 0 on success -- including a slice whose devices[] is empty, which
///         means "this module fronts no devices" -- or -1 on malformed input.
int hes_devreg_parse(hes_devreg_t* reg, const json_t* root);

////////////////////////////////////////////////////////////////////////////////
/// Looks up a device by its gateway deviceIndex.
///
/// @param reg The registry to search.
/// @param device_index The 'di' to find.
/// @return The device, or NULL if this module does not front it.
const hes_device_t* hes_devreg_find(const hes_devreg_t* reg, uint32_t device_index);

////////////////////////////////////////////////////////////////////////////////
/// Finds the object (by lexicon path) within a device.
///
/// @param dev The device to search.
/// @param path The object path to find.
/// @return The object, or NULL if the device does not expose it.
const hes_dev_object_t* hes_dev_object_find(const hes_device_t* dev, const char* path);

////////////////////////////////////////////////////////////////////////////////
/// Does the device expose an object with at least the given access bit at this
/// path?
///
/// @param dev The device to search.
/// @param path The object path.
/// @param access 'r' or 'w'.
/// @return 1 when the object exists and allows that access, 0 otherwise.
int hes_dev_object_can(const hes_device_t* dev, const char* path, char access);

////////////////////////////////////////////////////////////////////////////////
/// Collects the distinct object paths across ALL of the module's devices (the
/// topics the module must subscribe to).
///
/// @param reg The registry to walk.
/// @param topics Receives the paths, at most max of them.
/// @param max Capacity of topics.
/// @return The number of distinct paths written.
int hes_devreg_collect_topics(const hes_devreg_t* reg, char topics[][HES_PATH_MAX], int max);

#endif  // #ifndef OPENHES_SRC_COMMON_HES_DEVREG_H
