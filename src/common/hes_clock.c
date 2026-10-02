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
/// @brief The gateway clock: the offset the time service publishes, and the
/// corrected-UTC readers built on it (ISO/IEC 18012-3 11.2.3.1).
///
/// @details
/// One idea in two steps: atomically store the offset NTP measured, then add it
/// to CLOCK_REALTIME on every read. hes_clock.h explains why the host clock is
/// corrected rather than stepped.

#include "hes_clock.h"

#include <stdatomic.h>

#define NS_PER_SEC 1000000000LL

/// The gateway's correction to CLOCK_REALTIME. Atomic because the time service's
/// sync thread writes it while the module's bus loop reads it for every send.
static _Atomic long long g_offset_ns = 0;

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

void hes_clock_set_offset_ns(long long offset_ns)
{
    atomic_store(&g_offset_ns, offset_ns);
}

long long hes_clock_offset_ns(void)
{
    return atomic_load(&g_offset_ns);
}

uint64_t hes_clock_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }

    long long ns = (long long)ts.tv_sec * NS_PER_SEC + (long long)ts.tv_nsec;
    ns += atomic_load(&g_offset_ns);
    if (ns < 0) {
        return 0;  // before 1970: not representable, and not a real gateway time
    }
    return (uint64_t)ns;
}

time_t hes_clock_now(void)
{
    return (time_t)(hes_clock_now_ns() / (uint64_t)NS_PER_SEC);
}
