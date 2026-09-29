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
/// @brief A small SNTP (RFC 5905) client -- the 'nt' time source of the time
/// service (ISO/IEC 18012-3 11.2.3.1, Figure 12).
///
/// @details
/// 11.2.3.1: "The time service synchronizes to time (and date) from a range of
/// time sources." Table 56 lists the codes for those sources -- this file
/// produces the 'nt' (internet, NTP) one. A server that answers is a time source
/// in exactly the standard's sense; the choice among them is the "time source
/// map" and is made by the caller (services/time/time_sync.c), which tries them
/// in order.
///
/// Only the four-timestamp exchange is implemented (no NTP peer modes, no
/// authentication, no NTS). The client sends one request and reads one reply:
///
///     T1 = our transmit time      T2 = server receive time
///     T4 = our receive time       T3 = server transmit time
///
///     offset = ((T2 - T1) + (T3 - T4)) / 2      (server minus local)
///     delay  = (T4 - T1) - (T3 - T2)
///
/// The packet math is separated from the socket work (sntp_compute_offset() takes
/// plain byte buffers) so it can be unit-tested offline -- see
/// tests/test_time_net.c.
///
/// Known limit: NTP's 32-bit second field wraps in 2036. The standard era is
/// assumed, so a reply dated before 1970 or after 2036 is rejected rather than
/// silently misread.

#ifndef OPENHES_SRC_SERVICES_TIME_SNTP_H
#define OPENHES_SRC_SERVICES_TIME_SNTP_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/// An SNTP packet is exactly 48 bytes in both directions.
#define SNTP_PACKET_LEN 48

/// Seconds between the NTP epoch (1900-01-01) and the UNIX epoch (1970-01-01).
#define SNTP_UNIX_EPOCH_DELTA 2208988800LL

////////////////////////////////////////////////////////////////////////////////
/// One reply that passed every check.
typedef struct sntp_result {
    long long offset_ns;  ///< server time - local CLOCK_REALTIME, nanoseconds
    long delay_ms;        ///< round-trip delay
    int stratum;          ///< 1..15 (16 = unsynchronized, 0 = kiss-o'-death)
    int leap;             ///< 0..2 (3 = server's clock is not synchronized)
    time_t server_time;   ///< the server's transmit time, corrected to UNIX
} sntp_result_t;

////////////////////////////////////////////////////////////////////////////////
/// Build a client request (mode 3) stamped with 'now'.
///
/// The transmit timestamp written here is what a valid reply must echo back, so
/// the caller must keep 'packet' around to pass to sntp_compute_offset().
void sntp_build_request(uint8_t packet[SNTP_PACKET_LEN], struct timespec now);

////////////////////////////////////////////////////////////////////////////////
/// Validate a reply and compute the clock offset. Pure: no sockets, no clock.
///
/// Rejects a reply that is short, not a server mode-4 packet, from a server that
/// is unsynchronized (leap = 3), from stratum 0 (a kiss-o'-death) or stratum 16,
/// that fails to echo the request's transmit timestamp (an off-path forgery),
/// whose timestamps are internally inconsistent, or whose resulting offset is
/// more than a day -- i.e. everything a merely wrong answer looks like.
///
/// @param request     The request as sent by sntp_build_request().
/// @param reply       The bytes received.
/// @param reply_len   How many bytes were received (>= SNTP_PACKET_LEN).
/// @param local_recv  Local CLOCK_REALTIME reading taken when the reply arrived.
/// @param out         Receives the offset; untouched unless the call succeeds.
/// @return 0 when the reply is usable, -1 otherwise.
int sntp_compute_offset(const uint8_t request[SNTP_PACKET_LEN],
                        const uint8_t reply[SNTP_PACKET_LEN],
                        size_t reply_len,
                        struct timespec local_recv,
                        sntp_result_t* out);

////////////////////////////////////////////////////////////////////////////////
/// Query one NTP server over UDP port 123.
///
/// Blocking, with a receive timeout; call it from the sync thread, never from the
/// module's bus loop.
///
/// @param server Host name or address, e.g. "pool.ntp.org".
/// @return 0 when a usable reply arrived, -1 otherwise (reason logged).
int sntp_query(const char* server, int timeout_ms, sntp_result_t* out);

#endif  // #ifndef OPENHES_SRC_SERVICES_TIME_SNTP_H
