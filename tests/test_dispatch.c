////////////////////////////////////////////////////////////////////////////////
/// @file
/// @brief Offline test for local message routing and the two ways a client can
/// address one datum (ISO/IEC 18012-4).
///
/// @details
/// The same data can be asked for three ways, and all three must agree:
///
///   get /lx/ob/so/id              ->  the whole object   "rq=ma;si=ye"
///   get /lx/ob/so/id/rq           ->  one datum          "ma"
///   get /lx/ob/so/id?da=rq,si     ->  several data       "rq=ma;si=ye"
///
/// Two objects are registered, one nested inside the other's address, so the
/// longest-prefix rule is exercised for real:
///
///   /lx/ob/so/id                (configurationData)
///   /lx/ob/so/id/co/st/cv       (centralOperations)
///
/// Uses the REAL vendored nng: the dispatcher sends its GET answers on a pub
/// socket listening on inproc://, and a subscribed socket captures them, so the
/// message actually travels the same path as in production.
///
/// Build/run: see tests/CMakeLists.txt (ctest).
#include "common/hes_dispatch.h"

#include "common/hes_bus.h"
#include "common/hes_common.h"
#include "common/service_object.h"

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAPTURE_URL "inproc://test_dispatch"

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

static hes_bus_t g_bus;
static nng_socket g_cap;
static int g_puts;

////////////////////////////////////////////////////////////////////////////////
// two stand-in service objects, answering the POC's payload convention
////////////////////////////////////////////////////////////////////////////////

static void config_on_get(service_object_t* so, hes_clme_msg_t* out)
{
    (void)so;
    hes_msg_set_payload_str(out, "rq=ma;si=ye");
}

static void status_on_get(service_object_t* so, hes_clme_msg_t* out)
{
    (void)so;
    hes_msg_set_payload_str(out, "pi=abc;vr=1.0.0;nh=2");
}

static void any_on_put(service_object_t* so, const hes_clme_msg_t* in)
{
    (void)so;
    (void)in;
    g_puts++;
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
/// Sends one GET and returns the payload that came back ("" when none did).
static const char* get(const char* path, const char* query)
{
    static char payload[HES_PAYLOAD_MAX];
    static service_object_t config;
    static service_object_t status;
    service_object_t* objs[] = {&config, &status};
    hes_clme_msg_t out;
    size_t sz = sizeof(out);

    hes_strlcpy(config.path, sizeof(config.path), "/lx/ob/so/id");
    config.on_get = config_on_get;
    config.on_put = any_on_put;
    hes_strlcpy(status.path, sizeof(status.path), "/lx/ob/so/id/co/st/cv");
    status.on_get = status_on_get;
    status.on_put = any_on_put;

    hes_clme_msg_t in = {0};
    in.verb = HES_VERB_GET;
    hes_msg_set_path(&in, path);
    hes_strlcpy(in.query, sizeof(in.query), query ? query : "");

    memset(payload, 0, sizeof(payload));
    if (dispatch_to_local_objects(&g_bus, objs, 2, &in) == 0) {
        return payload;  // nothing owns this address
    }

    if (nng_recv(g_cap, &out, &sz, 0) != 0) {
        fprintf(stderr, "test: no answer for %s\n", path);
        exit(1);
    }
    hes_strlcpy(payload, sizeof(payload), out.payload);
    return payload;
}

int main(void)
{
    cap_start();

    ////////////////////////////////////////////////////////////////////////////////
    // the object's own address: all of its data
    ////////////////////////////////////////////////////////////////////////////////
    CHECK(strcmp(get("/lx/ob/so/id", NULL), "rq=ma;si=ye") == 0);
    printf("test: /lx/ob/so/id                 -> rq=ma;si=ye\n");

    ////////////////////////////////////////////////////////////////////////////////
    // the data-item form: append the transCode
    ////////////////////////////////////////////////////////////////////////////////
    CHECK(strcmp(get("/lx/ob/so/id/rq", NULL), "rq=ma") == 0);
    CHECK(strcmp(get("/lx/ob/so/id/si", NULL), "si=ye") == 0);
    printf("test: /lx/ob/so/id/rq              -> rq=ma\n");

    ////////////////////////////////////////////////////////////////////////////////
    // the 'da' query: several data points in one request
    ////////////////////////////////////////////////////////////////////////////////
    CHECK(strcmp(get("/lx/ob/so/id", "da=rq,si"), "rq=ma;si=ye") == 0);
    CHECK(strcmp(get("/lx/ob/so/id", "da=si"), "si=ye") == 0);
    CHECK(strcmp(get("/lx/ob/so/id", "da=si,rq"), "rq=ma;si=ye") == 0);  // order is the object's
    CHECK(strcmp(get("/lx/ob/so/id", "da= rq , si "), "rq=ma;si=ye") == 0);
    printf("test: /lx/ob/so/id?da=rq,si        -> rq=ma;si=ye (order independent)\n");

    ////////////////////////////////////////////////////////////////////////////////
    // longest prefix wins: the nested object, not its parent
    ////////////////////////////////////////////////////////////////////////////////
    CHECK(strcmp(get("/lx/ob/so/id/co/st/cv", NULL), "pi=abc;vr=1.0.0;nh=2") == 0);
    CHECK(strcmp(get("/lx/ob/so/id/co/st/cv/pi", NULL), "pi=abc") == 0);
    CHECK(strcmp(get("/lx/ob/so/id/co/st/cv", "da=vr,nh"), "vr=1.0.0;nh=2") == 0);
    printf("test: /lx/ob/so/id/co/st/cv/pi     -> pi=abc (longest prefix wins)\n");

    // Using both forms at once is a client error: the address is the more
    // specific, so it wins and the query is ignored (with a diagnostic).
    CHECK(strcmp(get("/lx/ob/so/id/co/st/cv/pi", "da=vr,nh"), "pi=abc") == 0);
    printf("test: address selector beats a conflicting 'da=' query\n");

    ////////////////////////////////////////////////////////////////////////////////
    // asking for something that is not there answers nothing
    ////////////////////////////////////////////////////////////////////////////////
    CHECK(strcmp(get("/lx/ob/so/id/nope", NULL), "") == 0);
    CHECK(strcmp(get("/lx/ob/so/id", "da=nope"), "") == 0);
    CHECK(strcmp(get("/lx/ob/so/id", "da="), "") == 0);
    printf("test: unknown datum / empty 'da='  -> empty answer\n");

    ////////////////////////////////////////////////////////////////////////////////
    // a nested address we do not serve is not routed to a parent
    ////////////////////////////////////////////////////////////////////////////////
    CHECK(strcmp(get("/lx/ob/so/id/co/st", NULL), "") == 0);
    CHECK(strcmp(get("/lx/ob/uo/li/ll/da/cv", NULL), "") == 0);
    printf("test: unserved nested address and a foreign path are not routed\n");

    ////////////////////////////////////////////////////////////////////////////////
    // PUT reaches the owning object, unchanged
    ////////////////////////////////////////////////////////////////////////////////
    {
        static service_object_t config;
        service_object_t* objs[] = {&config};

        hes_strlcpy(config.path, sizeof(config.path), "/lx/ob/so/id");
        config.on_put = any_on_put;

        hes_clme_msg_t in = {0};
        in.verb = HES_VERB_PUT;
        hes_msg_set_path(&in, "/lx/ob/so/id/rq");
        g_puts = 0;
        CHECK(dispatch_to_local_objects(&g_bus, objs, 1, &in) == 1);
        CHECK(g_puts == 1);
        printf("test: PUT /lx/ob/so/id/rq          -> delivered to the object\n");
    }

    nng_close(g_cap);
    nng_close(g_bus.pub_sock);
    printf("ALL TESTS PASSED\n");
    return 0;
}
