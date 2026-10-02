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
/// @brief Entry point of the Device Manifest service: a "system" component that
/// hands each interface module its own slice of the device registry.
///
/// @details
/// The service is implemented in dm.c; this header exposes only the launcher that
/// src/cli/cmd_dm.c calls. See docs/poc_design.md sections 9.3-9.4 and 12 for the model: a
/// static Product Profile Manifest (the virtual half) plus an in-memory registry
/// of the real devices modules report, served over NNG.

#ifndef OPENHES_SRC_DM_DM_H
#define OPENHES_SRC_DM_DM_H

////////////////////////////////////////////////////////////////////////////////
/// Launches the device manifest service, which listens for interface module
/// queries and reports, and maintains the registry of real devices.
///
/// @param profile Path to the profile.json file (virtual device expectations).
/// @param rep_url URL for the REQ/REP socket to listen on.
/// @param pub_url URL for the PUB socket to listen on.
/// @return 0 on success, non-zero on failure.
int manifest_service_main(const char* profile, const char* rep_url, const char* pub_url);

#endif  // #ifndef OPENHES_SRC_DM_DM_H
