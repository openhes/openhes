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
/// @brief The gateway's single "real time clock" (ISO/IEC 18012-3 11.2.3.1).
///
/// @details
/// "The
/// 'real time clock' keeps track of the time (Universal Coordinated Time - UTC)
/// and is available to other services via HES-CLME".
///
/// It is deliberately a *correction* on top of the host's CLOCK_REALTIME rather
/// than a write to the host's clock:
///
///   - Writing the system clock needs root/CAP_SYS_TIME, which a gateway
///     application should not require.
///   - Stepping the host clock backwards would make every timer in the process
///     (including the binding map's) jump, and can even repeat a second.
///
/// So the time service's time source (NTP, see services/time/sntp.h) measures an
/// offset, hes_clock_set_offset_ns() publishes it here, and every reader gets
/// corrected UTC. With no sync ever performed the offset stays 0 and this is
/// exactly CLOCK_REALTIME.
///
/// 18012-3 11.2.3.1 also requires a "high-resolution time stamping" block for
/// every HES-CLME message "to ensure proper sequence of events within the HES
/// gateway system" -- hes_clock_now_ns() is that block, and hes_bus_send()
/// stamps every outgoing message with it.

#ifndef OPENHES_SRC_COMMON_HES_CLOCK_H
#define OPENHES_SRC_COMMON_HES_CLOCK_H

#include <stdint.h>
#include <time.h>

////////////////////////////////////////////////////////////////////////////////
/// Sets the correction to apply to CLOCK_REALTIME. 0 restores the untouched
/// host clock.
///
/// @param offset_ns The correction, in nanoseconds.
void hes_clock_set_offset_ns(long long offset_ns);

////////////////////////////////////////////////////////////////////////////////
/// @return The correction currently in force, in nanoseconds.
long long hes_clock_offset_ns(void);

////////////////////////////////////////////////////////////////////////////////
/// @return Corrected UTC, whole seconds since the UNIX epoch (the time
///         service's 'va').
time_t hes_clock_now(void);

////////////////////////////////////////////////////////////////////////////////
/// @return Corrected UTC with nanosecond resolution, as stamped on HES-CLME
///         messages.
uint64_t hes_clock_now_ns(void);

#endif  // #ifndef OPENHES_SRC_COMMON_HES_CLOCK_H
