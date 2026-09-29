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
/// @brief A minimal blocking HTTPS GET, used by the time service to discover the
/// local time zone (see services/time/geo_tz.h).
///
/// @details
/// Why not plain HTTP: the answer decides how the gateway displays local time,
/// and an on-path attacker could redirect it.
///
/// The exchange itself is libcurl (vendored under deps/curl, built with the
/// vendored mbed TLS as its TLS backend). Letting it do the work buys the CA
/// store lookup, SNI, chunked replies, redirect policy and the timeout for a
/// handful of setopt calls, instead of a hand-rolled socket/TLS/chunk state
/// machine.
///
/// The server certificate is ALWAYS verified against the host's CA store. There
/// is deliberately no "insecure"/"skip verification" switch: a caller that
/// cannot verify must treat the request as failed, not as trusted.
///
/// This is a blocking, one-shot client: it is called from the time service's
/// background sync thread (services/time/time_sync.c), never from the module's
/// bus loop.
///
/// Memory/ownership: no handle, and nothing for the caller to release -- the
/// response lands in the caller's own buffer. libcurl's global state is
/// initialized once and never torn down.
///
/// Threading: blocking and safe from a background thread; see above for why it
/// must not run on the module's bus loop.

#ifndef OPENHES_SRC_COMMON_HES_HTTPS_H
#define OPENHES_SRC_COMMON_HES_HTTPS_H

#include <stddef.h>

/// Largest response entity this client returns. The gateway only ever asks for
/// small JSON documents, so a fixed buffer keeps the client allocation-free.
#define HES_HTTPS_BODY_MAX 4096

////////////////////////////////////////////////////////////////////////////////
/// Fetch an https:// URL and return its response entity.
///
/// @param url        Absolute URL, e.g. "https://ipapi.co/json/". Only https://
///                   is accepted; the port defaults to 443.
/// @param body       Receives the entity (the part after the response headers),
///                   NUL-terminated. Response headers are NOT included.
/// @param body_cap   Capacity of body; must be at least 2. An entity that does
///                   not fit is an error, never a silent truncation.
/// @param body_len   Optional; receives the entity length in bytes.
/// @param timeout_ms Budget for the whole exchange (DNS, connect, TLS
///                   handshake, request, response).
/// @return 0 when the request completed with a 2xx status AND the server
///         certificate verified; -1 otherwise (reason logged).
int hes_https_get(const char* url, char* body, size_t body_cap, size_t* body_len, int timeout_ms);

#endif  // #ifndef OPENHES_SRC_COMMON_HES_HTTPS_H
