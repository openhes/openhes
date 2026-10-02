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
/// @brief Implementation of the SNTP client (sntp.h): one request/reply over UDP,
/// and the packet math behind the offset.
///
/// @details
/// The packet math is deliberately separated from the socket work:
/// sntp_compute_offset() takes plain byte buffers, which is what lets it be
/// unit-tested offline (tests/test_time_net.c) with no network at all.
///
/// The endian helpers hide NTP's big-endian timestamp pairs. A reply that implies
/// more than a day of correction, or a round trip longer than a minute, is
/// rejected as a wrong or hostile answer rather than applied to the clock
/// (SNTP_MAX_OFFSET_NS, SNTP_MAX_DELAY_NS).

#include "sntp.h"

#include <log.h>

#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define NS_PER_SEC 1000000000LL

/// A reply that implies more than a day of correction, or a round trip longer
/// than a minute, is not a slow server -- it is a wrong or hostile answer.
#define SNTP_MAX_OFFSET_NS (86400LL * NS_PER_SEC)
#define SNTP_MAX_DELAY_NS (60LL * NS_PER_SEC)

/// NTP packet field offsets (RFC 5905 Figure 8).
#define NTP_ORIGINATE_TS 24
#define NTP_RECEIVE_TS 32
#define NTP_TRANSMIT_TS 40

////////////////////////////////////////////////////////////////////////////////
/// Reads a 32-bit value in big-endian order, which is the order NTP puts its
/// packet fields in (RFC 5905).
///
/// @param p The four bytes to read.
/// @return The value they encode.
static uint32_t read_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

////////////////////////////////////////////////////////////////////////////////
/// Writes a 32-bit value in big-endian order, the order NTP puts its packet
/// fields in (RFC 5905).
///
/// @param p The four bytes to write.
/// @param v The value to encode.
static void write_be32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

////////////////////////////////////////////////////////////////////////////////
/// Converts a struct timespec to nanoseconds since the UNIX epoch, which is the
/// unit the offset maths works in.
///
/// @param ts The time to convert.
/// @return The same instant, in nanoseconds.
static long long timespec_to_ns(struct timespec ts)
{
    return (long long)ts.tv_sec * NS_PER_SEC + (long long)ts.tv_nsec;
}

////////////////////////////////////////////////////////////////////////////////
/// Writes an NTP timestamp (RFC 5905): seconds since 1900 in the first four
/// bytes, the fraction of a second in the next four.
///
/// A time before the UNIX epoch is carried into the seconds field, so the
/// fraction always stays in [0, 1).
///
/// @param unix_ns Nanoseconds since the UNIX epoch.
/// @param out     Receives the 8-byte NTP timestamp.
static void unix_ns_to_ntp_ts(long long unix_ns, uint8_t* out)
{
    long long sec = unix_ns / NS_PER_SEC;
    long long rem_ns = unix_ns % NS_PER_SEC;
    if (rem_ns < 0) {
        rem_ns += NS_PER_SEC;
        sec -= 1;
    }
    write_be32(out, (uint32_t)(sec + SNTP_UNIX_EPOCH_DELTA));
    write_be32(out + 4, (uint32_t)((rem_ns << 32) / NS_PER_SEC));
}

////////////////////////////////////////////////////////////////////////////////
/// Converts an NTP 64-bit fixed-point timestamp to nanoseconds since the UNIX
/// epoch.
///
/// @return 0 on success, -1 for a value in the wrong NTP era (the second field
///         is only 32 bits wide, so it repeats every 136 years).
static int ntp_ts_to_unix_ns(const uint8_t* p, long long* out_ns)
{
    uint32_t sec = read_be32(p);
    uint32_t frac = read_be32(p + 4);

    if ((long long)sec < SNTP_UNIX_EPOCH_DELTA) {
        return -1;  // before 1970: not the era we are in
    }

    long long ns = ((long long)sec - SNTP_UNIX_EPOCH_DELTA) * NS_PER_SEC;
    ns += ((long long)frac * NS_PER_SEC) >> 32;
    *out_ns = ns;
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Public API
////////////////////////////////////////////////////////////////////////////////

void sntp_build_request(uint8_t packet[SNTP_PACKET_LEN], struct timespec now)
{
    memset(packet, 0, SNTP_PACKET_LEN);

    // LI = 0 (no warning), VN = 4, Mode = 3 (client). Stratum, poll interval,
    // precision and the root/reference fields stay zero: a client has nothing
    // meaningful to put there (RFC 5905 7.3).
    packet[0] = (uint8_t)((4 << 3) | 3);
    unix_ns_to_ntp_ts(timespec_to_ns(now), packet + NTP_TRANSMIT_TS);
}

int sntp_compute_offset(const uint8_t request[SNTP_PACKET_LEN],
                        const uint8_t reply[SNTP_PACKET_LEN],
                        size_t reply_len,
                        struct timespec local_recv,
                        sntp_result_t* out)
{
    if (out == NULL || reply_len < SNTP_PACKET_LEN) {
        return -1;
    }

    int leap = reply[0] >> 6;
    int mode = reply[0] & 0x07;
    int stratum = reply[1];

    if (mode != 4) {
        return -1;  // not a server reply (mode 4)
    }
    if (leap == 3) {
        return -1;  // the server's own clock is not synchronized
    }
    if (stratum == 0) {
        return -1;  // kiss-o'-death: the server is refusing us
    }
    if (stratum > 15) {
        return -1;  // 16 = unsynchronized
    }

    // The reply must echo the exact transmit timestamp we sent; without this an
    // off-path attacker could inject a plausible reply (RFC 5905 8).
    if (memcmp(reply + NTP_ORIGINATE_TS, request + NTP_TRANSMIT_TS, 8) != 0) {
        return -1;
    }

    long long t1;
    long long t2;
    long long t3;
    if (ntp_ts_to_unix_ns(request + NTP_TRANSMIT_TS, &t1) != 0) {
        return -1;
    }
    if (ntp_ts_to_unix_ns(reply + NTP_RECEIVE_TS, &t2) != 0) {
        return -1;
    }
    if (ntp_ts_to_unix_ns(reply + NTP_TRANSMIT_TS, &t3) != 0) {
        return -1;
    }
    long long t4 = timespec_to_ns(local_recv);

    if (t3 < t2) {
        return -1;  // the server claims to have sent before it received
    }

    long long delay = (t4 - t1) - (t3 - t2);
    if (delay < 0 || delay > SNTP_MAX_DELAY_NS) {
        return -1;
    }

    long long offset = ((t2 - t1) + (t3 - t4)) / 2;
    if (offset > SNTP_MAX_OFFSET_NS || offset < -SNTP_MAX_OFFSET_NS) {
        return -1;
    }

    out->offset_ns = offset;
    out->delay_ms = (long)(delay / 1000000LL);
    out->stratum = stratum;
    out->leap = leap;
    out->server_time = (time_t)(t3 / NS_PER_SEC);
    return 0;
}

int sntp_query(const char* server, int timeout_ms, sntp_result_t* out)
{
    if (server == NULL || out == NULL) {
        return -1;
    }
    if (timeout_ms <= 0) {
        timeout_ms = 3000;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    struct addrinfo* res = NULL;
    int gai = getaddrinfo(server, "123", &hints, &res);
    if (gai != 0) {
        log_error("sntp: cannot resolve %s: %s", server, gai_strerror(gai));
        return -1;
    }

    int sec = timeout_ms / 1000;
    int usec = (timeout_ms % 1000) * 1000;
    if (sec == 0 && usec == 0) {
        sec = 1;
    }
    struct timeval tv;
    tv.tv_sec = sec;
    tv.tv_usec = usec;

    int rc = -1;
    for (struct addrinfo* ai = res; ai != NULL && rc != 0; ai = ai->ai_next) {
        int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0) {
            continue;
        }

        // Connecting a UDP socket makes the kernel drop datagrams from anyone
        // else, which is the first line of defence against off-path injection.
        if (connect(s, ai->ai_addr, ai->ai_addrlen) != 0) {
            close(s);
            continue;
        }
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct timespec t1;
        clock_gettime(CLOCK_REALTIME, &t1);

        uint8_t request[SNTP_PACKET_LEN];
        sntp_build_request(request, t1);

        if (send(s, request, sizeof(request), MSG_NOSIGNAL) != (ssize_t)sizeof(request)) {
            close(s);
            continue;
        }

        uint8_t reply[SNTP_PACKET_LEN];
        ssize_t n = recv(s, reply, sizeof(reply), 0);
        struct timespec t4;
        clock_gettime(CLOCK_REALTIME, &t4);
        close(s);

        if (n < (ssize_t)SNTP_PACKET_LEN) {
            if (n < 0) {
                log_debug("sntp: no answer from %s (%s)", server, strerror(errno));
            }
            continue;
        }

        int rv = sntp_compute_offset(request, reply, (size_t)n, t4, out);
        if (rv == 0) {
            rc = 0;
        } else {
            log_debug("sntp: %s answered with an unusable packet", server);
        }
    }

    freeaddrinfo(res);
    return rc;
}
