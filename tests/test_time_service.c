////////////////////////////////////////////////////////////////////////////////
/// @file
/// @brief Offline test for the time service (ISO/IEC 18012-3 11.2.3) and for the
/// binding map's ability to read its answers.
///
/// @details
/// Three things are checked, all without a network:
///
///   1. The three functional objects exist at the three addresses of Tables 45,
///      49 and 53, and each answers with every transCode of the tables that
///      share its address (Tables 46-48, 50-52, 54-56).
///   2. A datum can be singled out of those answers -- both by appending its
///      transCode to the address and with '?da=' -- through the real router.
///      This is what used to fail: the old object answered a bare number, so
///      ".../cv/va" matched nothing and came back empty.
///   3. The binding map reads the value of the 'va' datum out of such a payload,
///      which is what makes a time-based rule work.
///
/// The sync state is supplied by a stub, so the test never touches the network
/// and the zone/offset values are exactly known.
///
/// Build/run: see tests/CMakeLists.txt (ctest).
#include "bm/bm.h"
#include "common/hes_bus.h"
#include "common/hes_common.h"
#include "common/hes_dispatch.h"
#include "common/service_object.h"
#include "services/time/time.h"

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CAPTURE_URL "inproc://test_time_service"

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

static hes_bus_t g_bus;
static nng_socket g_cap;
static time_snapshot_t g_snap;

////////////////////////////////////////////////////////////////////////////////
/// The sync state the objects see: whatever the test last put in g_snap.
static void stub_snapshot(void* ctx, time_snapshot_t* out)
{
    (void)ctx;
    *out = g_snap;
}

static void cap_start(void)
{
    memset(&g_bus, 0, sizeof(g_bus));

    if (nng_pub0_open(&g_bus.pub_sock) != 0 ||
        nng_listen(g_bus.pub_sock, CAPTURE_URL, NULL, 0) != 0 || nng_sub0_open(&g_cap) != 0 ||
        nng_socket_set(g_cap, NNG_OPT_SUB_SUBSCRIBE, "", 0) != 0 ||
        nng_dial(g_cap, CAPTURE_URL, NULL, 0) != 0) {
        fprintf(stderr, "test: cannot set up the capture bus\n");
        exit(1);
    }
    nng_socket_set_ms(g_cap, NNG_OPT_RECVTIMEO, 300);
    nng_msleep(50);
}

////////////////////////////////////////////////////////////////////////////////
/// The value of one datum in a 'transCode=value;...' answer, or NULL when the
/// answer does not carry it. Matching is per record, so 'pr' is never found
/// inside 'pr=100' by a search for 'r'.
static const char* datum_value(const char* payload, const char* code)
{
    static char value[64];
    size_t clen = strlen(code);
    const char* p = payload;

    while (*p != '\0') {
        const char* end = strchr(p, ';');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        const char* eq = memchr(p, '=', len);

        if (eq != NULL && (size_t)(eq - p) == clen && strncmp(p, code, clen) == 0) {
            size_t vlen = len - clen - 1;
            if (vlen >= sizeof(value)) {
                vlen = sizeof(value) - 1;
            }
            memcpy(value, eq + 1, vlen);
            value[vlen] = '\0';
            return value;
        }

        if (end == NULL) {
            break;
        }
        p = end + 1;
    }
    return NULL;
}

////////////////////////////////////////////////////////////////////////////////
/// True when the datum exists and holds exactly 'want'.
static int datum_is(const char* payload, const char* code, const char* want)
{
    const char* v = datum_value(payload, code);
    return v != NULL && strcmp(v, want) == 0;
}

////////////////////////////////////////////////////////////////////////////////
// 1. the three addresses and their tables
////////////////////////////////////////////////////////////////////////////////

static int check_realtime(service_object_t* so)
{
    time_t before = time(NULL);
    hes_clme_msg_t out = {0};
    so->on_get(so, &out);
    time_t after = time(NULL);

    const char* p = out.payload;

    // Table 46, the pre-market configurationData ('ro').
    CHECK(datum_is(p, "vr", "1.0.0"));
    CHECK(datum_is(p, "dt", "ss"));  // secondsSinceEpoch
    CHECK(datum_is(p, "ne", "1"));
    CHECK(datum_is(p, "um", "se"));  // seconds
    CHECK(datum_is(p, "mp", "me"));  // measured
    CHECK(datum_is(p, "rq", "op"));  // optional
    CHECK(datum_is(p, "pt", "tm"));  // time
    CHECK(datum_is(p, "ac", "in"));
    CHECK(datum_value(p, "sz") != NULL);
    CHECK(datum_value(p, "sc") != NULL);
    CHECK(datum_value(p, "cd") != NULL);
    CHECK(datum_value(p, "cp") != NULL);

    // Table 47, the post-market configurationData ('po').
    CHECK(datum_is(p, "mn", "0"));
    CHECK(datum_is(p, "mx", "4294967295"));
    CHECK(datum_is(p, "vl", "0"));
    CHECK(datum_is(p, "rl", "0"));

    // Table 48, the interactiveData ('ra'): the corrected clock, inside the
    // range Table 47 declares.
    const char* va = datum_value(p, "va");
    CHECK(va != NULL);
    long long epoch = atoll(va);
    CHECK(epoch >= (long long)before && epoch <= (long long)after);
    return 0;
}

static int check_localtz(service_object_t* so)
{
    // Table 52 'va' is "<UTC offset minutes>,<daylight minutes>".
    g_snap.tz_valid = 1;
    g_snap.std_offset_min = -240;
    g_snap.dst_offset_min = 60;

    hes_clme_msg_t out = {0};
    so->on_get(so, &out);

    const char* p = out.payload;
    CHECK(datum_is(p, "dt", "ai,ai"));  // two integers
    CHECK(datum_is(p, "ne", "2"));
    CHECK(datum_is(p, "um", "mi,mi"));  // minutes, minutes
    CHECK(datum_is(p, "mp", "pr"));     // preset, not measured
    CHECK(datum_is(p, "df", "0,0"));    // Table 51 default
    CHECK(datum_is(p, "va", "-240,60"));

    // Before the zone is known, Table 51's own default is reported rather than
    // a made-up offset.
    g_snap.tz_valid = 0;
    hes_msg_init(&out);
    so->on_get(so, &out);
    CHECK(datum_is(out.payload, "va", "0,0"));
    return 0;
}

static int check_source(service_object_t* so)
{
    // Table 56 'va' names where the time comes from.
    g_snap.ntp_valid = 1;

    hes_clme_msg_t out = {0};
    so->on_get(so, &out);
    CHECK(datum_is(out.payload, "dt", "sl"));
    CHECK(datum_is(out.payload, "ne", "1"));
    CHECK(datum_is(out.payload, "df", "lx"));
    CHECK(datum_is(out.payload, "va", "nt"));  // internet, NTP

    // Never synced: the local crystal, which is also Table 55's default.
    g_snap.ntp_valid = 0;
    hes_msg_init(&out);
    so->on_get(so, &out);
    CHECK(datum_is(out.payload, "va", "lx"));
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// 2. reaching one datum through the real router
////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////
/// Sends one GET through the router and returns the captured payload.
static const char* routed_get(service_object_t** objs, int n, const char* path, const char* query)
{
    static char payload[HES_PAYLOAD_MAX];
    hes_clme_msg_t out;
    size_t sz = sizeof(out);

    hes_clme_msg_t in = {0};
    in.verb = HES_VERB_GET;
    hes_msg_set_path(&in, path);
    if (query != NULL) {
        hes_strlcpy(in.query, sizeof(in.query), query);
    }

    memset(payload, 0, sizeof(payload));
    if (dispatch_to_local_objects(&g_bus, objs, n, &in) == 0) {
        return payload;  // nobody owns this address
    }

    if (nng_recv(g_cap, &out, &sz, 0) != 0) {
        fprintf(stderr, "test: no answer for %s\n", path);
        exit(1);
    }
    hes_strlcpy(payload, sizeof(payload), out.payload);
    return payload;
}

////////////////////////////////////////////////////////////////////////////////
// 3. the binding map reads the 'va' datum
////////////////////////////////////////////////////////////////////////////////

static int check_bm_reads_datums(void)
{
    // A bare payload is the value itself: this is how the interface modules
    // report a sensor, and it must keep working.
    CHECK(fabs(bm_datum_value("23.5", "va") - 23.5) < 1e-9);
    CHECK(fabs(bm_datum_value("1", "va") - 1.0) < 1e-9);
    CHECK(fabs(bm_datum_value("0", "va") - 0.0) < 1e-9);

    // A table answer: the 'va' record is the value, not the first record.
    CHECK(fabs(bm_datum_value("vr=1.0.0;va=1785612345", "va") - 1785612345.0) < 1e-9);
    CHECK(fabs(bm_datum_value("va=1785612345;vr=1.0.0", "va") - 1785612345.0) < 1e-9);

    // A table answer that does not carry 'va' must not hand back 'vr'.
    CHECK(fabs(bm_datum_value("vr=1.0.0;dt=ss", "va") - 0.0) < 1e-9);
    CHECK(fabs(bm_datum_value("", "va") - 0.0) < 1e-9);
    return 0;
}

int main(void)
{
    cap_start();

    time_service_config_t cfg;
    cfg.snapshot = stub_snapshot;
    cfg.snapshot_ctx = NULL;

    memset(&g_snap, 0, sizeof(g_snap));

    // The realTime object's clock is the host clock here: hes_clock's correction
    // is 0 unless a sync thread sets one.
    service_object_t* objs[TIME_SERVICE_OBJECT_COUNT];
    CHECK(time_service_create(&cfg, objs, TIME_SERVICE_OBJECT_COUNT) == TIME_SERVICE_OBJECT_COUNT);
    CHECK(strcmp(objs[TIME_OBJECT_REALTIME]->path, "/lx/ob/so/ti/rt/st/cv") == 0);
    CHECK(strcmp(objs[TIME_OBJECT_LOCALTZ]->path, "/lx/ob/so/ti/tz/st/cv") == 0);
    CHECK(strcmp(objs[TIME_OBJECT_SOURCE]->path, "/lx/ob/so/ti/st/mp/cv") == 0);

    CHECK(check_realtime(objs[TIME_OBJECT_REALTIME]) == 0);
    CHECK(check_localtz(objs[TIME_OBJECT_LOCALTZ]) == 0);
    CHECK(check_source(objs[TIME_OBJECT_SOURCE]) == 0);

    // Only realTime ticks, so only it carries the autonomous behaviour.
    CHECK(objs[TIME_OBJECT_REALTIME]->tick != NULL);

    // A PUT is ignored (18012-4 5.2.9.3.4: configurationData is not
    // client-updatable) -- and it must not disturb the reported clock.
    hes_clme_msg_t put = {0};
    put.verb = HES_VERB_PUT;
    hes_msg_set_path(&put, HES_LX_TIME_REALTIME_CV);
    hes_msg_set_payload_str(&put, "va=0");
    objs[TIME_OBJECT_REALTIME]->on_put(objs[TIME_OBJECT_REALTIME], &put);

    hes_clme_msg_t after = {0};
    objs[TIME_OBJECT_REALTIME]->on_get(objs[TIME_OBJECT_REALTIME], &after);
    CHECK(atoll(datum_value(after.payload, "va")) > 1000000000LL);

    ////////////////////////////////////////////////////////////////////////////////
    // one datum, two ways, through the router
    ////////////////////////////////////////////////////////////////////////////////
    g_snap.tz_valid = 1;
    g_snap.std_offset_min = -240;
    g_snap.dst_offset_min = 60;
    g_snap.ntp_valid = 1;

    const char* whole = routed_get(objs, TIME_SERVICE_OBJECT_COUNT, HES_LX_TIME_REALTIME_CV, NULL);
    CHECK(datum_value(whole, "vr") != NULL);
    CHECK(datum_value(whole, "va") != NULL);

    // Address the datum directly ...
    const char* one =
            routed_get(objs, TIME_SERVICE_OBJECT_COUNT, HES_LX_TIME_REALTIME_CV "/va", NULL);
    CHECK(strncmp(one, "va=", 3) == 0);

    // ... or ask for it with 'da'. The answer keeps the object's own order
    // ('mx' is defined before 'va' in Table 47/48), not the client's.
    const char* two =
            routed_get(objs, TIME_SERVICE_OBJECT_COUNT, HES_LX_TIME_REALTIME_CV, "da=va,mx");
    CHECK(strncmp(two, "mx=4294967295;va=", 17) == 0);

    // A datum the object does not have is empty, never a full dump.
    const char* none =
            routed_get(objs, TIME_SERVICE_OBJECT_COUNT, HES_LX_TIME_REALTIME_CV, "da=zz");
    CHECK(none[0] == '\0');

    // The zone object works the same way.
    const char* tz_va =
            routed_get(objs, TIME_SERVICE_OBJECT_COUNT, HES_LX_TIME_LOCALTZ_CV "/va", NULL);
    CHECK(strcmp(tz_va, "va=-240,60") == 0);

    ////////////////////////////////////////////////////////////////////////////////
    // the autonomous event-reports
    ////////////////////////////////////////////////////////////////////////////////
    objs[TIME_OBJECT_REALTIME]->tick(objs[TIME_OBJECT_REALTIME], &g_bus);

    int saw_realtime = 0;
    int saw_source = 0;
    for (int i = 0; i < 3; i++) {
        hes_clme_msg_t ev;
        size_t sz = sizeof(ev);
        if (nng_recv(g_cap, &ev, &sz, 0) != 0) {
            break;
        }
        CHECK(ev.verb == HES_VERB_EVENT);
        CHECK(strcmp(ev.query, "va") == 0);
        if (strcmp(ev.path, HES_LX_TIME_REALTIME_CV) == 0) {
            CHECK(datum_value(ev.payload, "va") != NULL);
            saw_realtime = 1;
        }
        if (strcmp(ev.path, HES_LX_TIME_SOURCE_CV) == 0) {
            CHECK(datum_is(ev.payload, "va", "nt"));
            saw_source = 1;
        }
    }
    CHECK(saw_realtime);
    CHECK(saw_source);

    CHECK(check_bm_reads_datums() == 0);

    for (int i = 0; i < TIME_SERVICE_OBJECT_COUNT; i++) {
        objs[i]->destroy(objs[i]);
        free(objs[i]);
    }

    nng_close(g_cap);
    hes_bus_close(&g_bus);

    printf("test_time_service: ok\n");
    return 0;
}
