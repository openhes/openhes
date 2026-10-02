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
/// @brief Entry point of the WiFi HAN interface module (`ohmg han wifi`).
///
/// @details
/// A launcher (src/cli/cmd_han_wifi.c) supplies the manifest service endpoints,
/// this module's moduleRefIndex, the two hub endpoints and the bulb's IP. The
/// module itself -- its A.1 duties, the manifest handshake and the WiZ polling --
/// is documented in wifi.c and README.md.

#ifndef OPENHES_SRC_HAN_WIFI_WIFI_H
#define OPENHES_SRC_HAN_WIFI_WIFI_H

#include <stdint.h>

int han_wifi_main(const char* svc_rep,
                  const char* svc_pub,
                  uint32_t module_ref,
                  const char* hub_pub_url,
                  const char* hub_sub_url,
                  const char* bulb_ip);

#endif  // #ifndef OPENHES_SRC_HAN_WIFI_WIFI_H
