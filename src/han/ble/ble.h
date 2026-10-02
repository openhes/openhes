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
/// @brief Entry point of the BLE HAN interface module (`ohmg han ble`).
///
/// @details
/// A launcher (src/cli/cmd_han_ble.c) supplies the manifest service endpoints,
/// this module's moduleRefIndex, the two hub endpoints and an optional MAC
/// override. The module itself -- its A.1 duties, the manifest handshake, presence
/// and the value publishing -- is documented in ble.c and README.md.

#ifndef OPENHES_SRC_HAN_BLE_BLE_H
#define OPENHES_SRC_HAN_BLE_BLE_H

#include <stdint.h>

////////////////////////////////////////////////////////////////////////////////
/// Launches the BLE HAN module.
///
/// @param svc_rep The manifest query/report service URL
/// @param svc_pub The manifest notifications service URL
/// @param module_ref The module reference index
/// @param hub_pub_url The hub publication service URL
/// @param hub_sub_url The hub subscription service URL
/// @param mac_override The MAC address override for simulated SensorTags
/// @return 0 on success, non-zero on failure
int han_ble_main(const char* svc_rep,
                 const char* svc_pub,
                 uint32_t module_ref,
                 const char* hub_pub_url,
                 const char* hub_sub_url,
                 const char* mac_override);

#endif  // #ifndef OPENHES_SRC_HAN_BLE_BLE_H
