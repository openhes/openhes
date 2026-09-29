////////////////////////////////////////////////////////////////////////////////
/// @file
/// @brief Offline test for routing data through the customer-specific protected
/// app (src/bm/bm.c app-service bridge).
///
/// @details
/// Scenarios:
///   tests/bm_appservice_button.xml      -- one input: button (di=6) -> light
///   tests/bm_appservice_multi_input.xml -- three inputs: button AND motion AND
///                                         darkness -> light
///
///   `op="ap"` is the appService operation (18012-3 Table 26): the OPERATION
///   is performed by the app, while the binding map keeps doing the routing
///   and the PUT. The app is identified by the row's refId, which is passed to
///   the script as hes_op(ref_id, inputs).
///
///   The built-in operators take at most two operands; the standard does not
///   bound an appService, so every input of the row is handed to the app. The
///   multi-input fixture is what makes AND/OR logic (impossible to express
///   with the built-in op set) testable.
///
/// The XML deliberately uses moduleType 'it' for nothing here: 'it' keeps its
/// meaning of "internal process" for ordinary chaining values, so it is not
/// used as an app address (see src/bm/README.md).
///
/// No Lua is involved: binding_map_t's hook is a plain function pointer, so
/// this test installs a C stub that stands in for the script and asserts that
/// the engine routes through it.
///
/// Build/run: see tests/CMakeLists.txt (ctest), or compile with the same
/// includes/libs as test_bm.c.
#include "../src/bm/bm.h"

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTN_PATH "/lx/ob/uo/ui/ud/da/cv"
#define LIGHT_PATH "/lx/ob/uo/li/ll/da/cv"
#define MOTION_PATH "/lx/ob/uo/ui/um/da/cv"
#define DARK_PATH "/lx/ob/uo/hv/ls/as/cv"

#define MAX_CAPTURED 16

static hes_bus_t g_bus;   // the bus we hand to the binding map
static nng_socket g_cap;  // our capture subscriber

////////////////////////////////////////////////////////////////////////////////
// stand-in for the Lua app
////////////////////////////////////////////////////////////////////////////////

// Stub for the app: records every operand and returns a value that is
// deliberately NOT the raw input, proving the app's result is what reaches the
// light. ref_id 1 mirrors the single-input rule; other rows are treated as
// "AND of all operands", which is the logic the built-in operators cannot
// express.
static int g_op_calls;
static uint32_t g_op_last_ref_id;
static int g_op_last_n_operands;
static uint32_t g_op_last_dis[BM_MAX_INPUTS];
static double g_op_last_values[BM_MAX_INPUTS];

static int stub_app_operation(void* ctx,
                              const bm_operation_t* op,
                              const bm_operand_t* operands,
                              int n_operands,
                              double* result)
{
    (void)ctx;
    g_op_calls++;
    g_op_last_ref_id = op->ref_id;
    g_op_last_n_operands = n_operands;
    for (int i = 0; i < n_operands && i < BM_MAX_INPUTS; i++) {
        g_op_last_dis[i] = operands[i].device_index;
        g_op_last_values[i] = operands[i].value;
    }

    if (op->ref_id == 1) {
        *result = (operands[0].value != 0.0) ? 5.0 : 0.0;
        return 0;
    }

    int all_set = 1;
    for (int i = 0; i < n_operands; i++) {
        if (operands[i].value == 0.0) {
            all_set = 0;
        }
    }
    *result = all_set ? 1.0 : 0.0;
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// capture plumbing (same approach as test_bm.c)
////////////////////////////////////////////////////////////////////////////////

static void cap_start(void)
{
    memset(&g_bus, 0, sizeof(g_bus));

    if (nng_pub0_open(&g_bus.pub_sock) != 0) {
        fprintf(stderr, "test: nng_pub0_open failed\n");
        exit(1);
    }
    if (nng_listen(g_bus.pub_sock, "inproc://test_bm_appservice", NULL, 0) != 0) {
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
    if (nng_dial(g_cap, "inproc://test_bm_appservice", NULL, 0) != 0) {
        fprintf(stderr, "test: inproc dial failed\n");
        exit(1);
    }
    nng_socket_set_ms(g_cap, NNG_OPT_RECVTIMEO, 300);
    nng_msleep(50);  // let the inproc subscription settle
}

static int expect_sends(int n, hes_clme_msg_t* out)
{
    int got = 0;
    for (int i = 0; i < n; i++) {
        size_t sz = sizeof(hes_clme_msg_t);
        if (nng_recv(g_cap, &out[got], &sz, 0) != 0) {
            fprintf(stderr, "test: timeout waiting for msg %d of %d\n", i, n);
            return -1;
        }
        got++;
    }
    hes_clme_msg_t extra;
    size_t sz = sizeof(extra);
    if (nng_recv(g_cap, &extra, &sz, 0) == 0) {
        fprintf(stderr, "test: unexpected extra send: verb=%u path=%s payload=%s\n",
                (unsigned)extra.verb, extra.path, extra.payload);
        return -1;
    }
    return got;
}

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

////////////////////////////////////////////////////////////////////////////////
/// Single-input appService: the row's operation is performed by the app.
static int test_single_input(void)
{
    printf("--- appService with one input ---\n");

    binding_map_t bm;
    bm_init(&bm);
    CHECK(bm_load_xml(&bm, "bm_appservice_button.xml") == 0);
    CHECK(bm.n_ops == 1);
    CHECK(strcmp(bm.ops[0].operation, "ap") == 0);
    CHECK(strcmp(bm.ops[0].inputs[0].source_object, BTN_PATH) == 0);
    CHECK(bm.ops[0].out_device_index == 2);
    CHECK(strcmp(bm.ops[0].out_dest_object, LIGHT_PATH) == 0);

    bm.app_ctx = NULL;
    bm.app_operation = stub_app_operation;

    cap_start();

    hes_clme_msg_t msgs[MAX_CAPTURED];
    bm_controller_start(&bm, &g_bus);
    CHECK(expect_sends(1, msgs) == 1);
    CHECK(msgs[0].verb == HES_VERB_SUBSCRIBE);
    CHECK(strcmp(msgs[0].path, BTN_PATH) == 0);

    // Button down: the engine asks the app for the operation's result.
    g_op_calls = 0;
    feed(&bm, BTN_PATH, 1.0);
    CHECK(expect_sends(1, msgs) == 1);
    CHECK(is_put(&msgs[0], LIGHT_PATH, 2, "5"));  // the app's 5, not the raw 1
    CHECK(g_op_calls == 1);
    CHECK(g_op_last_ref_id == 1);
    CHECK(g_op_last_n_operands == 1);
    CHECK(g_op_last_dis[0] == 6);
    CHECK(g_op_last_values[0] == 1.0);
    printf("test: button=1 -> app returned 5 -> PUT %s = %s\n", msgs[0].path, msgs[0].payload);

    // Button up.
    feed(&bm, BTN_PATH, 0.0);
    CHECK(expect_sends(1, msgs) == 1);
    CHECK(is_put(&msgs[0], LIGHT_PATH, 2, "0"));
    CHECK(g_op_calls == 2);

    // Change detection still applies to the app's result.
    feed(&bm, BTN_PATH, 0.0);
    CHECK(expect_sends(0, msgs) == 0);
    printf("test: unchanged app result -> no re-send\n");

    nng_close(g_cap);
    nng_close(g_bus.pub_sock);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Multi-input appService: one row, three inputs, AND logic in the app.
static int test_multi_input(void)
{
    printf("--- appService with three inputs ---\n");

    binding_map_t bm;
    bm_init(&bm);
    CHECK(bm_load_xml(&bm, "bm_appservice_multi_input.xml") == 0);
    CHECK(bm.n_ops == 1);
    CHECK(strcmp(bm.ops[0].operation, "ap") == 0);
    CHECK(bm.ops[0].n_inputs == 3);
    CHECK(bm.ops[0].inputs[0].device_index == 6);
    CHECK(bm.ops[0].inputs[1].device_index == 7);
    CHECK(bm.ops[0].inputs[2].device_index == 8);

    bm.app_ctx = NULL;
    bm.app_operation = stub_app_operation;

    cap_start();

    hes_clme_msg_t msgs[MAX_CAPTURED];
    bm_controller_start(&bm, &g_bus);
    // One SUBSCRIBE per external input: button, motion, light level.
    CHECK(expect_sends(3, msgs) == 3);
    printf("test: controller subscribed to %d inputs\n", 3);

    g_op_calls = 0;

    // Only one of three inputs known: the row must not fire yet.
    feed(&bm, BTN_PATH, 1.0);
    CHECK(expect_sends(0, msgs) == 0);
    feed(&bm, MOTION_PATH, 1.0);
    CHECK(expect_sends(0, msgs) == 0);
    CHECK(g_op_calls == 0);
    printf("test: partial inputs -> app not called\n");

    // Third input arrives: the app now sees all three operands.
    feed(&bm, DARK_PATH, 1.0);
    CHECK(expect_sends(1, msgs) == 1);
    CHECK(is_put(&msgs[0], LIGHT_PATH, 2, "1"));
    CHECK(g_op_calls == 1);
    CHECK(g_op_last_ref_id == 2);
    CHECK(g_op_last_n_operands == 3);
    CHECK(g_op_last_dis[0] == 6);
    CHECK(g_op_last_dis[1] == 7);
    CHECK(g_op_last_dis[2] == 8);
    CHECK(g_op_last_values[0] == 1.0);
    CHECK(g_op_last_values[1] == 1.0);
    CHECK(g_op_last_values[2] == 1.0);
    printf("test: 3 operands reached the app -> PUT %s = %s\n", msgs[0].path, msgs[0].payload);

    // It stops being dark: the app re-decides with the new operand.
    feed(&bm, DARK_PATH, 0.0);
    CHECK(expect_sends(1, msgs) == 1);
    CHECK(is_put(&msgs[0], LIGHT_PATH, 2, "0"));
    CHECK(g_op_calls == 2);

    // Same operands again: the app is not re-invoked.
    feed(&bm, DARK_PATH, 0.0);
    CHECK(expect_sends(0, msgs) == 0);
    CHECK(g_op_calls == 2);
    printf("test: unchanged operands -> app not re-invoked\n");

    nng_close(g_cap);
    nng_close(g_bus.pub_sock);
    return 0;
}

int main(void)
{
    if (test_single_input() != 0) {
        return 1;
    }
    if (test_multi_input() != 0) {
        return 1;
    }

    printf("ALL TESTS PASSED\n");
    return 0;
}
