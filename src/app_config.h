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
/// @brief Size limits and the NNG endpoints every command and module shares.
///
/// @details
/// A simple or complex integral gateway (docs/poc_design.md) runs on one
/// machine, so each bus is a Unix domain socket instead of a TCP port: nothing
/// is reachable from off-box, no port can clash with another service, and
/// there is no bind address to configure.
///
/// No stale-socket cleanup is needed here: if a killed process leaves its
/// socket file behind, NNG's ipc listener notices the dead file, unlinks it and
/// binds again (deps/nng, ipc_remove_stale()).

#ifndef OPENHES_SRC_APP_CONFIG_H
#define OPENHES_SRC_APP_CONFIG_H

#define MAX_PATH_SIZE 4096
#define MAX_URL_SIZE 2048
#define MAX_CMD_SIZE 32
#define MAX_ERR_MSG_SIZE 256

// ---------------------------------------------------------------------------
// Module identity
// ---------------------------------------------------------------------------

/// moduleType ('mt') a HAN-facing interface module declares when it asks the
/// Device Manifest service for its slice. A module's identity is the pair
/// (moduleType, moduleRefIndex), and a product profile numbers service modules
/// ('sm') and interface modules ('hi') independently -- both regularly carry
/// moduleRefIndex 1 -- so a request that gives the index alone can be answered
/// with the wrong module's slice.
#define HES_MT_INTERFACE "hi"

// ---------------------------------------------------------------------------
// NNG endpoints
// ---------------------------------------------------------------------------

/// Device Manifest service -- REQ/REP: a module queries its slice, reports presence.
#define HES_MANIFEST_REP_URL "ipc:///tmp/ohmg-device-manifest-rep.sock"

/// Device Manifest service -- PUB: notifications, subscribed by moduleRefIndex.
#define HES_MANIFEST_PUB_URL "ipc:///tmp/ohmg-device-manifest-pub.sock"

/// Core module (the hub) -- PUB: downstream HES-CLME; leaves dial in as SUB.
#define HES_HUB_PUB_URL "ipc:///tmp/ohmg-hub-pub.sock"

/// Core module (the hub) -- SUB: upstream HES-CLME; leaves dial in as PUB.
#define HES_HUB_SUB_URL "ipc:///tmp/ohmg-hub-sub.sock"

// ---------------------------------------------------------------------------
// HTTPS
// ---------------------------------------------------------------------------

/// Trust store the HTTPS client verifies against, passed to libcurl as
/// CURLOPT_CAINFO (see src/common/hes_https.c). It is named here rather than in
/// the HTTPS client because the build pins the same file as libcurl's default
/// (CURL_CA_BUNDLE, top-level CMakeLists.txt): a build that autodetects none --
/// as the arm64 cross build did -- verifies against an empty trust store, so the
/// two must agree.
#define HES_HTTPS_CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"

#endif  // #ifndef OPENHES_SRC_APP_CONFIG_H
