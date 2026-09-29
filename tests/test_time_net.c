////////////////////////////////////////////////////////////////////////////////
/// @file
/// @brief Offline tests for the time service's network-facing helpers: the SNTP
/// packet maths (ISO/IEC 18012-3 11.2.3.1 "time source & sync") and the time
/// zone lookup ("time zone & daylight map").
///
/// @details
/// Nothing here opens a socket. The SNTP tests hand sntp_compute_offset() a
/// hand-built reply, which is the only way to test the interesting cases
/// deterministically anyway -- a real server will not send you a kiss-o'-death
/// packet or a forged originate timestamp on request. The zone tests exercise
/// the JSON reader and the system time zone database directly.
///
/// Build/run: see tests/CMakeLists.txt (ctest).
#include "services/time/geo_tz.h"
#include "services/time/sntp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

#define NS_PER_SEC 1000000000LL

static int skipped;

////////////////////////////////////////////////////////////////////////////////
/// Is 'got' within 'tolerance' of 'want'?
///
/// An NTP timestamp's fraction is 32 bits, so encoding a value truncates it by
/// up to ~0.2 ns and a computed offset therefore lands a few nanoseconds away
/// from the ideal arithmetic. That is the wire format, not a bug, so the checks
/// below allow for it.
static int within(long long got, long long want, long long tolerance)
{
    long long d = got - want;
    if (d < 0) {
        d = -d;
    }
    return d <= tolerance;
}

////////////////////////////////////////////////////////////////////////////////
/// Writes an NTP 64-bit fixed-point timestamp for a UNIX time in nanoseconds.
static void put_ntp_ts(uint8_t* p, long long unix_ns)
{
    long long sec = unix_ns / NS_PER_SEC;
    long long rem = unix_ns % NS_PER_SEC;
    if (rem < 0) {
        rem += NS_PER_SEC;
        sec -= 1;
    }
    uint32_t ntp_sec = (uint32_t)(sec + SNTP_UNIX_EPOCH_DELTA);
    uint32_t frac = (uint32_t)((rem << 32) / NS_PER_SEC);

    p[0] = (uint8_t)(ntp_sec >> 24);
    p[1] = (uint8_t)(ntp_sec >> 16);
    p[2] = (uint8_t)(ntp_sec >> 8);
    p[3] = (uint8_t)ntp_sec;
    p[4] = (uint8_t)(frac >> 24);
    p[5] = (uint8_t)(frac >> 16);
    p[6] = (uint8_t)(frac >> 8);
    p[7] = (uint8_t)frac;
}

#define T1_SEC 1700000000LL
#define T1_NS (T1_SEC * NS_PER_SEC)

/// The scenario: the server is 5 s ahead of us, the network costs 20 ms each
/// way, and the server spends 1 ms answering.
#define NET_MS 20LL
#define SERVER_MS 1LL
#define SERVER_OFFSET_NS (5LL * NS_PER_SEC)

static long long t2_ns(long long skew_ns)
{
    return T1_NS + skew_ns + NET_MS * 1000000LL;
}
static long long t3_ns(long long skew_ns)
{
    return t2_ns(skew_ns) + SERVER_MS * 1000000LL;
}
static long long t4_ns(void)
{
    return T1_NS + (2 * NET_MS) * 1000000LL;
}

////////////////////////////////////////////////////////////////////////////////
/// Builds the request/reply pair of the scenario above.
static void build_exchange(uint8_t req[SNTP_PACKET_LEN],
                           uint8_t rep[SNTP_PACKET_LEN],
                           long long skew_ns)
{
    struct timespec send_at;
    send_at.tv_sec = T1_SEC;
    send_at.tv_nsec = 0;
    sntp_build_request(req, send_at);

    memset(rep, 0, SNTP_PACKET_LEN);
    rep[0] = (uint8_t)((0 << 6) | (4 << 3) | 4);  // LI 0, VN 4, mode 4 = server
    rep[1] = 2;                                   // stratum 2
    memcpy(rep + 24, req + 40, 8);                // originate := what we sent
    put_ntp_ts(rep + 32, t2_ns(skew_ns));         // server received
    put_ntp_ts(rep + 40, t3_ns(skew_ns));         // server transmitted
}

static struct timespec local_recv(void)
{
    struct timespec ts;
    long long ns = t4_ns();
    ts.tv_sec = (time_t)(ns / NS_PER_SEC);
    ts.tv_nsec = (long)(ns % NS_PER_SEC);
    return ts;
}

static int test_sntp_maths(void)
{
    uint8_t req[SNTP_PACKET_LEN];
    uint8_t rep[SNTP_PACKET_LEN];
    sntp_result_t r;

    build_exchange(req, rep, SERVER_OFFSET_NS);

    CHECK(sntp_compute_offset(req, rep, SNTP_PACKET_LEN, local_recv(), &r) == 0);

    // offset = ((T2-T1) + (T3-T4)) / 2. The round trip cancels out of it: the
    // symmetric 20 ms costs leave exactly the server's 5 s plus half of the
    // 1 ms the server spent answering.
    long long expected =
            ((t2_ns(SERVER_OFFSET_NS) - T1_NS) + (t3_ns(SERVER_OFFSET_NS) - t4_ns())) / 2;
    CHECK(within(r.offset_ns, expected, 100));
    CHECK(r.offset_ns > SERVER_OFFSET_NS);  // half a millisecond more
    CHECK(within(r.offset_ns - SERVER_OFFSET_NS, 500000LL, 100));
    CHECK(r.stratum == 2);
    CHECK(r.leap == 0);

    // delay = (T4-T1) - (T3-T2) = 40 ms round trip minus the 1 ms the server
    // spent. The millisecond report is a truncation of that, hence the unit of
    // slack.
    CHECK(within(r.delay_ms, 2 * NET_MS - SERVER_MS, 1));

    // server_time is the server's transmit time, Unix-based.
    CHECK(r.server_time == T1_SEC + 5);

    // A server that is behind us gives a negative offset, and the maths is
    // symmetric.
    build_exchange(req, rep, -SERVER_OFFSET_NS);
    CHECK(sntp_compute_offset(req, rep, SNTP_PACKET_LEN, local_recv(), &r) == 0);
    CHECK(r.offset_ns < 0);
    return 0;
}

static int test_sntp_rejects(void)
{
    uint8_t req[SNTP_PACKET_LEN];
    uint8_t rep[SNTP_PACKET_LEN];
    sntp_result_t r;
    struct timespec t4 = local_recv();

    // A usable reply, so each mutation below is the only thing wrong.
    build_exchange(req, rep, SERVER_OFFSET_NS);
    CHECK(sntp_compute_offset(req, rep, SNTP_PACKET_LEN, t4, &r) == 0);

    // Too short to be an SNTP packet at all.
    CHECK(sntp_compute_offset(req, rep, SNTP_PACKET_LEN - 1, t4, &r) != 0);

    // Not a server reply (mode 3 = client, 5 = broadcast).
    rep[0] = (uint8_t)((0 << 6) | (4 << 3) | 3);
    CHECK(sntp_compute_offset(req, rep, SNTP_PACKET_LEN, t4, &r) != 0);
    rep[0] = (uint8_t)((0 << 6) | (4 << 3) | 5);
    CHECK(sntp_compute_offset(req, rep, SNTP_PACKET_LEN, t4, &r) != 0);

    // The server says its own clock is not synchronized.
    rep[0] = (uint8_t)((3 << 6) | (4 << 3) | 4);
    CHECK(sntp_compute_offset(req, rep, SNTP_PACKET_LEN, t4, &r) != 0);

    // Kiss-o'-death (stratum 0) and stratum 16 (unsynchronized).
    rep[0] = (uint8_t)((0 << 6) | (4 << 3) | 4);
    rep[1] = 0;
    CHECK(sntp_compute_offset(req, rep, SNTP_PACKET_LEN, t4, &r) != 0);
    rep[1] = 16;
    CHECK(sntp_compute_offset(req, rep, SNTP_PACKET_LEN, t4, &r) != 0);

    // A reply that does not echo our transmit timestamp is not an answer to
    // our question -- this is the off-path forgery case.
    rep[1] = 2;
    rep[24] ^= 0x01;
    CHECK(sntp_compute_offset(req, rep, SNTP_PACKET_LEN, t4, &r) != 0);
    rep[24] ^= 0x01;
    CHECK(sntp_compute_offset(req, rep, SNTP_PACKET_LEN, t4, &r) == 0);

    // The server claims to have transmitted before it received.
    put_ntp_ts(rep + 40, t2_ns(SERVER_OFFSET_NS) - NS_PER_SEC);
    CHECK(sntp_compute_offset(req, rep, SNTP_PACKET_LEN, t4, &r) != 0);

    // An absurd correction (the answer is more than a day out) is refused
    // rather than applied.
    build_exchange(req, rep, 3LL * 86400 * NS_PER_SEC);
    CHECK(sntp_compute_offset(req, rep, SNTP_PACKET_LEN, t4, &r) != 0);
    return 0;
}

int main(void)
{
    CHECK(test_sntp_maths() == 0);
    CHECK(test_sntp_rejects() == 0);

    ////////////////////////////////////////////////////////////////////////////////
    // the zone name reader
    ////////////////////////////////////////////////////////////////////////////////
    char name[GEO_TZ_NAME_MAX];

    CHECK(geo_tz_parse_json("{\"timezone\":\"Asia/Taipei\"}", NULL, name, sizeof(name)) == 0);
    CHECK(strcmp(name, "Asia/Taipei") == 0);

    CHECK(geo_tz_parse_json("{\"timezone\":\"America/Argentina/Buenos_Aires\"}", NULL, name,
                            sizeof(name)) == 0);
    CHECK(strcmp(name, "America/Argentina/Buenos_Aires") == 0);

    // Some providers nest the answer.
    CHECK(geo_tz_parse_json("{\"data\":{\"timezone\":\"Europe/Paris\"}}", NULL, name,
                            sizeof(name)) == 0);
    CHECK(strcmp(name, "Europe/Paris") == 0);

    // Anything that is not a plain zone name is refused: this string ends up
    // in the TZ environment variable.
    CHECK(geo_tz_parse_json("{\"timezone\":\"../../etc/passwd\"}", NULL, name, sizeof(name)) != 0);
    CHECK(geo_tz_parse_json("{\"timezone\":\"Asia/Tai;rm -rf /\"}", NULL, name, sizeof(name)) != 0);
    CHECK(geo_tz_parse_json("{\"timezone\":\"/leading\"}", NULL, name, sizeof(name)) != 0);
    CHECK(geo_tz_parse_json("{\"timezone\":\"double//slash\"}", NULL, name, sizeof(name)) != 0);
    CHECK(geo_tz_parse_json("{\"timezone\":\"\"}", NULL, name, sizeof(name)) != 0);

    // Wrong member, wrong type, and not JSON at all.
    CHECK(geo_tz_parse_json("{\"utc_offset\":\"+08:00\"}", NULL, name, sizeof(name)) != 0);
    CHECK(geo_tz_parse_json("{\"timezone\":42}", NULL, name, sizeof(name)) != 0);
    CHECK(geo_tz_parse_json("<html>rate limited</html>", NULL, name, sizeof(name)) != 0);
    CHECK(geo_tz_parse_json("", NULL, name, sizeof(name)) != 0);

    // A caller-supplied key wins, for providers that name it differently.
    CHECK(geo_tz_parse_json("{\"zone\":\"Europe/Berlin\"}", "zone", name, sizeof(name)) == 0);
    CHECK(strcmp(name, "Europe/Berlin") == 0);

    ////////////////////////////////////////////////////////////////////////////////
    // the zone offsets, from the system database
    ////////////////////////////////////////////////////////////////////////////////

    // These two hold regardless of whether the host has a time zone database:
    // they fail on the name check or the missing-zone check, not on offsets.
    int std_min = 0;
    int dst_min = 0;
    CHECK(geo_tz_offsets("../etc/passwd", &std_min, &dst_min) != 0);
    CHECK(geo_tz_offsets("Not/AZone", &std_min, &dst_min) != 0);

    if (access("/usr/share/zoneinfo/Asia/Taipei", R_OK) != 0) {
        printf("test_time_net: no time zone database; offset checks skipped\n");
        skipped = 1;
    } else {
        // No daylight saving.
        CHECK(geo_tz_offsets("Asia/Taipei", &std_min, &dst_min) == 0);
        CHECK(std_min == 480 && dst_min == 0);

        CHECK(geo_tz_offsets("Asia/Kolkata", &std_min, &dst_min) == 0);
        CHECK(std_min == 330 && dst_min == 0);

        // Northern hemisphere: January is standard time, July is daylight.
        CHECK(geo_tz_offsets("America/New_York", &std_min, &dst_min) == 0);
        CHECK(std_min == -300 && dst_min == 60);

        CHECK(geo_tz_offsets("Europe/London", &std_min, &dst_min) == 0);
        CHECK(std_min == 0 && dst_min == 60);

        // Southern hemisphere: the sampling of both halves of the year is what
        // keeps the standard offset at +600 rather than +660.
        CHECK(geo_tz_offsets("Australia/Sydney", &std_min, &dst_min) == 0);
        CHECK(std_min == 600 && dst_min == 60);

        // A half-hour zone with a daylight saving rule worth noting.
        CHECK(geo_tz_offsets("Australia/Lord_Howe", &std_min, &dst_min) == 0);
        CHECK(std_min == 630 && dst_min == 30);
    }

    // The host fallback must not crash whatever the environment says.
    char host_zone[GEO_TZ_NAME_MAX];
    (void)geo_tz_system_name(host_zone, sizeof(host_zone));

    (void)skipped;
    printf("test_time_net: ok\n");
    return 0;
}
