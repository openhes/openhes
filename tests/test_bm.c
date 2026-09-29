////////////////////////////////////////////////////////////////////////////////
/// @file
/// @brief Offline unit test for the binding-map engine (src/bm/bm.c), exercising
/// it against the real (vendored) nng + libxml2 -- no stub.
///
/// @details
/// Why this exists / how it differs from poc2/tests:
///   - poc2 tested its own binding_map.c against a hand-rolled NNG stub that
///     recorded sends. The root tree builds against the real nng, so this test
///     instead captures the binding map's sends over a REAL inproc PUB/SUB pair:
///     we open a pub socket (handed to the binding map as its "bus"), listen on
///     inproc://, and dial a capture SUB subscribed to "" -- every message the
///     map sends is received and asserted on.
///   - The XML fixtures live in <root>/tests (bm_*_controlled_light.xml), NOT
///     the poc2 sample documents, so assertions target THIS scenario.
///
/// Scenario under test: tests/bm_button_controlled_light.xml
///   - one rule: SensorTag button (di=6) down  -> light (di=2) ON
///               SensorTag button (di=6) up    -> light (di=2) OFF
///   - controller: one SUBSCRIBE for the button object (the only external input)
///   - processor:  button=1 -> PUT light "1"; button=0 -> PUT light "0";
///                 unchanged value -> no re-send (change detection).
///
/// Build/run: see tests/CMakeLists.txt (ctest), or (one command):
///     cc -Wall -Wextra -I../src -I../deps/nng/include
///        -I../deps/libxml2/include test_bm.c ../src/bm/bm.c
///        ../src/common/hes_bus.c -lnng -lxml2 -lm -o test_bm
///     ./test_bm [path-to-bm_button_controlled_light.xml]
#include "../src/bm/bm.h"

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTN_PATH "/lx/ob/uo/ui/ud/da/cv"
#define LIGHT_PATH "/lx/ob/uo/li/ll/da/cv"

#define MAX_CAPTURED 16

static hes_bus_t g_bus;   // the bus we hand to the binding map
static nng_socket g_cap;  // our capture subscriber

////////////////////////////////////////////////////////////////////////////////
/// Build an inproc pub/sub so the binding map's hes_bus_send() calls land in
/// our capture socket (real nng, no stub).
static void cap_start(void)
{
    memset(&g_bus, 0, sizeof(g_bus));

    if (nng_pub0_open(&g_bus.pub_sock) != 0) {
        fprintf(stderr, "test: nng_pub0_open failed\n");
        exit(1);
    }
    if (nng_listen(g_bus.pub_sock, "inproc://test_bm_cap", NULL, 0) != 0) {
        fprintf(stderr, "test: inproc listen failed\n");
        exit(1);
    }

    if (nng_sub0_open(&g_cap) != 0) {
        fprintf(stderr, "test: nng_sub0_open failed\n");
        exit(1);
    }
    if (nng_socket_set(g_cap, NNG_OPT_SUB_SUBSCRIBE, "", 0) != 0) {
        fprintf(stderr, "test: subscribe-all failed\n");
        exit(1);
    }
    if (nng_dial(g_cap, "inproc://test_bm_cap", NULL, 0) != 0) {
        fprintf(stderr, "test: inproc dial failed\n");
        exit(1);
    }
    nng_socket_set_ms(g_cap, NNG_OPT_RECVTIMEO, 300);
    nng_msleep(50);  // let the inproc subscription settle
}

////////////////////////////////////////////////////////////////////////////////
/// Read exactly the n messages the last action should have produced.
/// Also checks that NO extra message arrived. Returns n on success, -1 on
/// timeout / mismatch / unexpected extra.
static int expect_sends(int n, hes_clme_msg_t* out)
{
    int got = 0;
    for (int i = 0; i < n; i++) {
        size_t sz = sizeof(hes_clme_msg_t);
        int rv = nng_recv(g_cap, &out[got], &sz, 0);
        if (rv != 0) {
            fprintf(stderr, "test: timeout waiting for msg %d of %d\n", i, n);
            return -1;
        }
        if (sz != sizeof(hes_clme_msg_t)) {
            fprintf(stderr, "test: bad message size %zu\n", sz);
            return -1;
        }
        got++;
    }
    // Ensure there is no unexpected extra send.
    hes_clme_msg_t extra;
    size_t sz = sizeof(extra);
    int rv = nng_recv(g_cap, &extra, &sz, 0);
    if (rv == 0) {
        fprintf(stderr, "test: unexpected extra send: verb=%u path=%s payload=%s\n",
                (unsigned)extra.verb, extra.path, extra.payload);
        return -1;
    }
    return got;
}

////////////////////////////////////////////////////////////////////////////////
/// Feed an event into the binding-map processor.
static void feed(binding_map_t* bm, const char* path, double v)
{
    hes_clme_msg_t in = {0};
    in.verb = HES_VERB_EVENT;
    hes_msg_set_path(&in, path);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f", v);
    hes_msg_set_payload_str(&in, buf);
    bm_processor_handle(bm, &g_bus, &in);
}

////////////////////////////////////////////////////////////////////////////////
/// Same, but stamped with a source deviceIndex to exercise (path, di)
/// attribution.
static void feed_dev(binding_map_t* bm, const char* path, uint32_t di, double v)
{
    hes_clme_msg_t in = {0};
    in.verb = HES_VERB_EVENT;
    in.device_index = di;
    hes_msg_set_path(&in, path);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f", v);
    hes_msg_set_payload_str(&in, buf);
    bm_processor_handle(bm, &g_bus, &in);
}

static int is_put(const hes_clme_msg_t* m, const char* path, uint32_t di, const char* payload)
{
    return m->verb == HES_VERB_PUT && m->device_index == di && strcmp(m->path, path) == 0 &&
           strcmp(m->payload, payload) == 0;
}

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

int main(int argc, char** argv)
{
    const char* xml = (argc > 1) ? argv[1] : "bm_button_controlled_light.xml";

    binding_map_t bm;
    bm_init(&bm);
    CHECK(bm_load_xml(&bm, xml) == 0);
    printf("test: loaded %s -> ops=%d addrs=%d\n", xml, bm.n_ops, bm.n_addrs);

    ////////////////////////////////////////////////////////////////////////////////
    // parse assertions
    ////////////////////////////////////////////////////////////////////////////////
    CHECK(bm.n_ops == 1);
    CHECK(bm.n_addrs == 2);
    const bm_operation_t* op = &bm.ops[0];
    CHECK(strcmp(op->operation, "gt") == 0);
    CHECK(op->enabled);
    CHECK(op->n_inputs == 1);
    CHECK(op->inputs[0].device_index == 6);
    CHECK(strcmp(op->inputs[0].source_object, BTN_PATH) == 0);
    CHECK(op->out_device_index == 2);
    CHECK(strcmp(op->out_dest_object, LIGHT_PATH) == 0);
    // addressing rows resolve both devices
    CHECK(bm_find_addr(&bm, 6) != NULL);
    CHECK(bm_find_addr(&bm, 2) != NULL);

    cap_start();

    ////////////////////////////////////////////////////////////////////////////////
    // controller: exactly one SUBSCRIBE, for the button (external)
    ////////////////////////////////////////////////////////////////////////////////
    hes_clme_msg_t msgs[MAX_CAPTURED];
    bm_controller_start(&bm, &g_bus);
    CHECK(expect_sends(1, msgs) == 1);
    CHECK(msgs[0].verb == HES_VERB_SUBSCRIBE);
    CHECK(msgs[0].device_index == 6);
    CHECK(strcmp(msgs[0].path, BTN_PATH) == 0);
    printf("test: controller subscribed to %s (di=6)\n", msgs[0].path);

    ////////////////////////////////////////////////////////////////////////////////
    // a question is not a reading: a GET request must not become a value
    ////////////////////////////////////////////////////////////////////////////////
    // The request carries only a path, so reading a value out of it would store
    // bm_datum_value("") == 0.0 as the button's value -- a reading never taken --
    // and fire the row. Nothing may be sent, and the input must stay unknown.
    {
        hes_clme_msg_t req = {0};
        req.verb = HES_VERB_GET;
        hes_msg_set_path(&req, BTN_PATH);
        bm_processor_handle(&bm, &g_bus, &req);
        CHECK(expect_sends(0, msgs) == 0);
    }

    // The same goes for an EVENT that carries no reading.
    {
        hes_clme_msg_t ev = {0};
        ev.verb = HES_VERB_EVENT;
        hes_msg_set_path(&ev, BTN_PATH);
        bm_processor_handle(&bm, &g_bus, &ev);
        CHECK(expect_sends(0, msgs) == 0);
    }
    printf("test: GET request / payload-less EVENT -> no value cached, no PUT\n");

    ////////////////////////////////////////////////////////////////////////////////
    // processor: button down -> light ON
    ////////////////////////////////////////////////////////////////////////////////
    feed_dev(&bm, BTN_PATH, 6, 1.0);
    CHECK(expect_sends(1, msgs) == 1);
    CHECK(is_put(&msgs[0], LIGHT_PATH, 2, "1"));
    printf("test: button down -> PUT %s = %s\n", msgs[0].path, msgs[0].payload);

    ////////////////////////////////////////////////////////////////////////////////
    // processor: button up -> light OFF
    ////////////////////////////////////////////////////////////////////////////////
    feed(&bm, BTN_PATH, 0.0);  // legacy no-di attribution path
    CHECK(expect_sends(1, msgs) == 1);
    CHECK(is_put(&msgs[0], LIGHT_PATH, 2, "0"));
    printf("test: button up   -> PUT %s = %s\n", msgs[0].path, msgs[0].payload);

    ////////////////////////////////////////////////////////////////////////////////
    // change detection: same value again -> no re-send
    ////////////////////////////////////////////////////////////////////////////////
    feed(&bm, BTN_PATH, 0.0);
    CHECK(expect_sends(0, msgs) == 0);
    printf("test: unchanged value -> no re-send (change detection works)\n");

    ////////////////////////////////////////////////////////////////////////////////
    // and it flips again
    ////////////////////////////////////////////////////////////////////////////////
    feed_dev(&bm, BTN_PATH, 6, 1.0);
    CHECK(expect_sends(1, msgs) == 1);
    CHECK(is_put(&msgs[0], LIGHT_PATH, 2, "1"));
    printf("test: button down again -> PUT %s = %s\n", msgs[0].path, msgs[0].payload);

    nng_close(g_cap);
    nng_close(g_bus.pub_sock);
    printf("ALL TESTS PASSED\n");
    return 0;
}
