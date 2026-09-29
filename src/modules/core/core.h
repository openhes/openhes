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
/// @brief The core service module: launch settings and entry point.
///
/// @details
/// The core module is the hub of the gateway. It hosts the binding map (the one
/// place that sees all HES-CLME traffic), the service objects (id, time, auth,
/// crypto), the Lexicon router and the customer-specific protected app. This
/// header carries only what a launcher needs -- core_config_t and core_main();
/// the module's own documentation is in README.md.
///
/// Memory/ownership: core_config_t holds borrowed pointers into the caller's
/// storage (the CLI's argv), so it needs no allocation and no destructor.
///
/// Threading: core_main() runs the module's bus loop in the calling thread; the
/// time service's NTP sync is the one background thread it starts.

#ifndef OPENHES_SRC_MODULES_CORE_CORE_H
#define OPENHES_SRC_MODULES_CORE_CORE_H

////////////////////////////////////////////////////////////////////////////////
/// Everything the core service module is launched with.
///
/// Grouped into a struct rather than passed as ten positional arguments: the
/// time service alone adds four, and a call site with ten bare strings is one
/// transposition away from a silent misconfiguration.
typedef struct core_config {
    const char* pub_url;  ///< PUB endpoint, for downstream modules to subscribe to
    const char* sub_url;  ///< SUB endpoint, for upstream modules to publish to
    const char* bm_xml;   ///< the binding map XML instance document
    const char* lua_app;  ///< the Lua script to run as the customer-specific app

    /// Path to the (read-only, secret) authorization policy document; NULL for
    /// an empty policy. The A&A service loads it once and propagates it into the
    /// binding map's authorType fields at startup.
    const char* auth_policy;

    /// 1 = ignore every authorization gate: the binding map neither honours its
    /// own authorType ('at') fields nor consults the per-message ('ap'='au')
    /// hook, so nothing can be suppressed on authorization grounds whatever the
    /// XML says or the policy writes. Nothing else changes -- the A&A service is
    /// still created and still answers, and the policy is still propagated, so a
    /// run with this set still shows what authorization *would* have done.
    ///
    /// A development switch (CLI: --no-authz), for a demo, a bisect or an
    /// investigation that must not trip over an authorization failure. Never on
    /// a gateway that is meant to enforce anything.
    int no_authz;

    /// Path to the identity document (identification service, holds the secret
    /// fingerprint). Mandatory: the module refuses to start without a valid
    /// one.
    const char* identity_path;

    ////////////////////////////////////////////////////////////////////////////////
    // gateway inventory (identification service, Table 42)
    ////////////////////////////////////////////////////////////////////////////////

    /// Device Manifest service REQ endpoint. The module list it holds is this
    /// gateway's own inventory -- the product profile declares the modules the
    /// box contains -- which is where the identification service's nh/nw/ns
    /// counts come from. NULL or "" means "no registry": the counts then fall
    /// back to the binding map's addressing table, which is a stand-in (one
    /// map's reach list, not the gateway's).
    const char* manifest_rep_url;

    ////////////////////////////////////////////////////////////////////////////////
    // time service (ISO/IEC 18012-3 11.2.3) -- all optional
    ////////////////////////////////////////////////////////////////////////////////

    const char* ntp_servers;  ///< CSV of NTP hosts; NULL = built-in defaults

    /// forced IANA zone (e.g. "Asia/Taipei"); NULL means "detect it, then ask
    /// the host"
    const char* tz_name;

    const char* tz_urls;  ///< CSV of zone providers; NULL = built-in defaults
    int ntp_enabled;      ///< 0 = never contact an NTP server
    int tz_enabled;       ///< 0 = never contact a zone provider
} core_config_t;

////////////////////////////////////////////////////////////////////////////////
/// Launches the core service module: binding map + service objects + the
/// customer-specific protected app (a Lua script).
///
/// @param cfg Launch settings; must not be NULL. See core_config_t.
/// @return 0 on success, non-zero on failure.
int core_main(const core_config_t* cfg);

#endif  // #ifndef OPENHES_SRC_MODULES_CORE_CORE_H
