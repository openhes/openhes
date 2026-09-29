////////////////////////////////////////////////////////////////////////////////
/// @file
/// @brief Offline test for the authorization service's propagation into the
/// binding map (ISO/IEC 18012-3 11.2.4).
///
/// @details
/// What is under test:
///
///   The authorization service does not inspect traffic. It writes each
///   applicable permission's valueNew ('vn') into the binding map's authorType
///   ('at') field named by that permission's lexiconObject ('lo'). The binding
///   map then enforces 'at' locally: 'bk' suppresses the outgoing PUT.
///
/// Fixtures:
///   tests/bm_auth_two_targets.xml  -- two rows, both authorType 'bk' by default
///                                     (deny by default); addresses
///                                     /lx/ob/bm/ot1/op1/at and .../ot2/op1/at
///   tests/auth_policy_kid.json     -- kid active (st=au): opens target A,
///                                     leaves target B blocked (Annex D's
///                                     "kid partial control")
///   tests/auth_policy_parent.json  -- parent active (st=au): opens both
///
/// Real nng + libxml2 + jansson; no Lua, no bus round-trip needed.
/// Build/run: see tests/CMakeLists.txt (ctest).
#include "../src/bm/bm.h"
#include "../src/services/auth/auth.h"

#include <nng/nng.h>
#include <nng/protocol/pubsub0/pub.h>
#include <nng/protocol/pubsub0/sub.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BTN_PATH "/lx/ob/uo/ui/ud/da/cv"
#define TARGET_A "/lx/ob/uo/li/ll/da/cv"  ///< light power, di=2, table ot1
#define TARGET_B "/lx/ob/uo/li/ll/aa/cv"  ///< light brightness, di=2, table ot2

#define MAX_CAPTURED 8

static hes_bus_t g_bus;
static nng_socket g_cap;

////////////////////////////////////////////////////////////////////////////////
// capture plumbing (same approach as the other binding-map tests)
////////////////////////////////////////////////////////////////////////////////

static void cap_start(void)
{
    memset(&g_bus, 0, sizeof(g_bus));

    if (nng_pub0_open(&g_bus.pub_sock) != 0) {
        fprintf(stderr, "test: nng_pub0_open failed\n");
        exit(1);
    }
    if (nng_listen(g_bus.pub_sock, "inproc://test_auth", NULL, 0) != 0) {
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
    if (nng_dial(g_cap, "inproc://test_auth", NULL, 0) != 0) {
        fprintf(stderr, "test: inproc dial failed\n");
        exit(1);
    }
    nng_socket_set_ms(g_cap, NNG_OPT_RECVTIMEO, 300);
    nng_msleep(50);
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
        fprintf(stderr, "test: unexpected extra send: path=%s payload=%s\n", extra.path,
                extra.payload);
        return -1;
    }
    return got;
}

static void feed(binding_map_t* bm, const char* path, uint32_t di, double v)
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

////////////////////////////////////////////////////////////////////////////////
/// The host-side writer: how the authorization service reaches the binding map.
/// Same process here, so it is a direct call (see core.c).
static int write_at(void* ctx, const char* at_address, const char* value)
{
    return bm_set_at((binding_map_t*)ctx, at_address, value);
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
/// Both rows start blocked: deny by default, before any authorization.
static int test_starts_denied(binding_map_t* bm)
{
    printf("--- binding map starts denied by default ---\n");

    CHECK(bm_load_xml(bm, "bm_auth_two_targets.xml") == 0);
    CHECK(bm->n_ops == 2);
    CHECK(strcmp(bm->ops[0].out_author_type, "bk") == 0);
    CHECK(strcmp(bm->ops[1].out_author_type, "bk") == 0);

    // The row addresses the authorization policy must name.
    char addr[HES_PATH_MAX];
    bm_at_address(&bm->ops[0], addr, sizeof(addr));
    CHECK(strcmp(addr, "/lx/ob/bm/ot1/op1/at") == 0);
    bm_at_address(&bm->ops[1], addr, sizeof(addr));
    CHECK(strcmp(addr, "/lx/ob/bm/ot2/op1/at") == 0);

    // Nothing flows while both are blocked.
    cap_start();
    hes_clme_msg_t msgs[MAX_CAPTURED];
    feed(bm, BTN_PATH, 6, 1.0);
    CHECK(expect_sends(0, msgs) == 0);
    feed(bm, BTN_PATH, 7, 1.0);
    CHECK(expect_sends(0, msgs) == 0);
    printf("test: both targets blocked before authorization\n");

    nng_close(g_cap);
    nng_close(g_bus.pub_sock);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Kid active: target A opens, target B stays blocked.
static int test_kid_partial(void)
{
    printf("--- kid active (st=au): partial control ---\n");

    binding_map_t bm;
    bm_init(&bm);
    CHECK(bm_load_xml(&bm, "bm_auth_two_targets.xml") == 0);

    service_object_t* auth = auth_service_create("auth_policy_kid.json");
    CHECK(auth != NULL);

    // A GET must expose class info but never a credential.
    hes_clme_msg_t reply = {0};
    auth->on_get(auth, &reply);
    printf("test: auth GET -> %s\n", reply.payload);
    CHECK(strstr(reply.payload, "kid") != NULL);
    CHECK(strstr(reply.payload, "secret") == NULL);
    CHECK(strstr(reply.payload, "parent-secret") == NULL);

    int writes = auth_service_propagate(auth, write_at, &bm);
    CHECK(writes == 2);  // kid's two permission rows (statusCheck 'au')

    CHECK(strcmp(bm.ops[0].out_author_type, "fl") == 0);  // target A opened
    CHECK(strcmp(bm.ops[1].out_author_type, "bk") == 0);  // target B blocked

    cap_start();
    hes_clme_msg_t msgs[MAX_CAPTURED];

    // Target A flows.
    feed(&bm, BTN_PATH, 6, 1.0);
    CHECK(expect_sends(1, msgs) == 1);
    CHECK(is_put(&msgs[0], TARGET_A, 2, "1"));
    printf("test: target A allowed -> PUT %s = %s\n", msgs[0].path, msgs[0].payload);

    // Target B is suppressed.
    feed(&bm, BTN_PATH, 7, 1.0);
    CHECK(expect_sends(0, msgs) == 0);
    printf("test: target B blocked -> no send\n");

    nng_close(g_cap);
    nng_close(g_bus.pub_sock);

    auth->destroy(auth);
    free(auth);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// Same permission tables, only the class statuses swapped: both open.
static int test_parent_full(void)
{
    printf("--- parent active (st=au): both targets ---\n");

    binding_map_t bm;
    bm_init(&bm);
    CHECK(bm_load_xml(&bm, "bm_auth_two_targets.xml") == 0);

    service_object_t* auth = auth_service_create("auth_policy_parent.json");
    CHECK(auth != NULL);

    int writes = auth_service_propagate(auth, write_at, &bm);
    CHECK(writes == 2);  // parent's two rows

    CHECK(strcmp(bm.ops[0].out_author_type, "fl") == 0);
    CHECK(strcmp(bm.ops[1].out_author_type, "fl") == 0);

    cap_start();
    hes_clme_msg_t msgs[MAX_CAPTURED];

    feed(&bm, BTN_PATH, 6, 1.0);
    CHECK(expect_sends(1, msgs) == 1);
    CHECK(is_put(&msgs[0], TARGET_A, 2, "1"));

    feed(&bm, BTN_PATH, 7, 1.0);
    CHECK(expect_sends(1, msgs) == 1);
    CHECK(is_put(&msgs[0], TARGET_B, 2, "1"));
    printf("test: both targets allowed for the parent\n");

    nng_close(g_cap);
    nng_close(g_bus.pub_sock);

    auth->destroy(auth);
    free(auth);
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// A permission naming a binding-map address that does not exist is reported,
/// not silently ignored.
static int test_unknown_address(void)
{
    printf("--- permission naming an unknown binding-map row ---\n");

    binding_map_t bm;
    bm_init(&bm);
    CHECK(bm_load_xml(&bm, "bm_auth_two_targets.xml") == 0);

    CHECK(bm_set_at(&bm, "/lx/ob/bm/ot9/op1/at", "fl") == -1);
    CHECK(bm_set_at(&bm, "/lx/ob/bm/ot1/op1/at", "fl") == 0);
    CHECK(strcmp(bm.ops[0].out_author_type, "fl") == 0);
    printf("test: unknown at address rejected\n");

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// The development switch (core's --no-authz): a row the policy left blocked
/// still flows once authorization is disabled -- and the 'at' field is *not*
/// rewritten, so the switch is honoured at the gate, not by editing the map.
static int test_authz_disabled(void)
{
    printf("--- --no-authz: a 'bk' row flows anyway ---\n");

    binding_map_t bm;
    bm_init(&bm);
    CHECK(bm_load_xml(&bm, "bm_auth_two_targets.xml") == 0);

    service_object_t* auth = auth_service_create("auth_policy_kid.json");
    CHECK(auth != NULL);

    // Kid active: target A opens, target B is left at 'bk'.
    int writes = auth_service_propagate(auth, write_at, &bm);
    CHECK(writes == 2);
    CHECK(strcmp(bm.ops[1].out_author_type, "bk") == 0);

    cap_start();
    hes_clme_msg_t msgs[MAX_CAPTURED];

    // Baseline: the blocked row really is suppressed.
    feed(&bm, BTN_PATH, 7, 1.0);
    CHECK(expect_sends(0, msgs) == 0);

    // Disable authorization, then feed a *different* value: the operand cache
    // recorded the refused press (only the output is left untraced), so the same
    // value again would be skipped as "no change" rather than gated.
    bm_disable_authorization(&bm);
    CHECK(strcmp(bm.ops[1].out_author_type, "bk") == 0);  // still 'bk': ignored, not rewritten
    feed(&bm, BTN_PATH, 7, 0.0);
    CHECK(expect_sends(1, msgs) == 1);
    CHECK(is_put(&msgs[0], TARGET_B, 2, "0"));
    printf("test: target B flowed once authorization was disabled (field still 'bk')\n");

    nng_close(g_cap);
    nng_close(g_bus.pub_sock);

    auth->destroy(auth);
    free(auth);
    return 0;
}

int main(void)
{
    binding_map_t bm;
    bm_init(&bm);

    if (test_starts_denied(&bm) != 0) {
        return 1;
    }
    if (test_kid_partial() != 0) {
        return 1;
    }
    if (test_parent_full() != 0) {
        return 1;
    }
    if (test_unknown_address() != 0) {
        return 1;
    }
    if (test_authz_disabled() != 0) {
        return 1;
    }

    printf("ALL TESTS PASSED\n");
    return 0;
}
