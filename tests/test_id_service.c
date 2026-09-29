////////////////////////////////////////////////////////////////////////////////
/// @file
/// @brief Offline test for the identification service (ISO/IEC 18012-3 11.2.2),
/// driven directly with no bus round-trip.
///
/// @details
/// Three things are being pinned down:
///
///   1. The service answers on three Lexicon addresses -- one per table group --
///      and the memoryType of each table decides what the answer may contain:
///
///        /lx/ob/so/id            Table 37 configurationData ('pr') rq, si
///        /lx/ob/so/id/co/st/cv   Tables 41/42/43/44 ('ro'/'op'/'sp'/'po')
///        /lx/ac/so/id/dc/bm/cv   Table 39 discovery        ('op')
///
///   2. The identity document is validated strictly. 'pi' and 'fp' are 256-bit
///      values (11.2.2.1) carried as 64 hex characters; a value that is short,
///      non-hex, or missing is refused outright. Nothing is padded, defaulted,
///      or regenerated -- 11.2.2.1 requires the public ID to be *stationary*, so
///      quietly minting a fresh one would be worse than not starting.
///
///   3. The fingerprint never leaves. Table 41 gives 'pi' memoryType 'ro'
///      (viewable) and Table 43 gives 'fp' 'sp' (obscured), so the status answer
///      carries the public ID and omits the secret entirely.
///
/// Fixtures are the identity*.json documents in this directory; ctest runs the
/// binary from here so the relative paths resolve.
///
/// Build/run: see tests/CMakeLists.txt (ctest), or compile with the same
/// includes/libs as test_auth_propagate.c.
#include "services/id/id.h"

#include "common/hes_common.h"
#include "common/service_object.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

////////////////////////////////////////////////////////////////////////////////
/// 'pi' is DERIVED from 'fp' now, so a fixture cannot carry the expected value
/// (that is the point: it is a key, not a constant). These helpers check the
/// shape of what is served instead.
static int is_lower_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

////////////////////////////////////////////////////////////////////////////////
/// True when the answer carries 'pi=' followed by 64 lowercase hex characters.
static int has_derived_pi(const char* payload)
{
    const char* p = strstr(payload, "pi=");
    if (!p) {
        return 0;
    }
    p += 3;
    for (int i = 0; i < 64; i++) {
        if (!is_lower_hex(p[i])) {
            return 0;
        }
    }
    return p[64] == ';' || p[64] == '\0';
}
#define DEMO_FP "9c41d2e83a7b0f5c6d1e8a9b4f203c7d5a6b1e9f0c8d3a7b2e5f4c1d9a8b7e6f"

/// How many leading characters of 'fp' must never show up anywhere. Testing a
/// prefix catches the realistic leak (a truncated or partially-masked value),
/// not just an exact copy.
#define FP_HEAD_CHARS 16

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

////////////////////////////////////////////////////////////////////////////////
/// Runs a GET the way core.c does -- request fields pre-populated, on_get
/// overwrites the payload -- and returns what the service answered.
static const char* get_payload(service_object_t* so)
{
    static char buf[HES_PAYLOAD_MAX];
    hes_clme_msg_t out = {0};
    out.verb = HES_VERB_GET;
    hes_msg_set_path(&out, so->path);
    so->on_get(so, &out);

    hes_strlcpy(buf, sizeof(buf), out.payload);
    return buf;
}

////////////////////////////////////////////////////////////////////////////////
/// Stands in for core.c's core_gateway_inventory(): the host is the only one
/// that knows the gateway's module inventory and binding maps.
static void fake_inventory(void* ctx, id_inventory_t* out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    out->hans_number = 2;
    out->wans_number = 1;
    out->service_modules_number = 1;
    out->n_binding_maps = 1;
    hes_strlcpy(out->binding_maps[0].module_type, sizeof(out->binding_maps[0].module_type), "sm");
    out->binding_maps[0].module_ref_index = 1;
    out->binding_maps[0].net_ref_index = 0;
}

int main(void)
{
    service_object_t* so[ID_SERVICE_OBJECT_COUNT];

    ////////////////////////////////////////////////////////////////////////////////
    // a well-formed document creates one object per address
    ////////////////////////////////////////////////////////////////////////////////
    CHECK(id_service_create("identity.json", so, ID_SERVICE_OBJECT_COUNT) ==
          ID_SERVICE_OBJECT_COUNT);
    CHECK(strcmp(so[0]->path, HES_LX_ID) == 0);
    CHECK(strcmp(so[1]->path, HES_LX_ID_CENTRALOPS_CV) == 0);
    CHECK(strcmp(so[2]->path, HES_LX_ID_DISCOVER_BM) == 0);
    printf("test: created the service at %s, %s and %s\n", so[0]->path, so[1]->path, so[2]->path);

    // Table 37: the capability declaration, 'pr' -- the standard's own
    // preassigned values, not configuration.
    const char* cfg = get_payload(so[0]);
    CHECK(strcmp(cfg, "rq=ma;si=ye") == 0);

    // Tables 41/42/44: served together from the one centralOperations address.
    // Without an inventory source the counts are honestly zero.
    const char* st = get_payload(so[1]);
    printf("test: GET %s -> %s\n", so[1]->path, st);
    CHECK(has_derived_pi(st));  // Table 41, 'ro': the public key derived from 'fp'
    CHECK(strstr(st, "vr=1.0.0") != NULL);
    CHECK(strstr(st, "hs=1") != NULL);
    CHECK(strstr(st, "ch=sh") != NULL);
    CHECK(strstr(st, "pd=Demo gateway") != NULL);
    CHECK(strstr(st, "vl=2;il=3;ll=2;rl=6") != NULL);  // Table 44, 'po'
    CHECK(strstr(st, "nh=0;nw=0;ns=0") != NULL);       // Table 42 before wiring

    // 'fp' is 'sp' / obscured: not the whole value, not a prefix of it, and
    // not even under its own transCode.
    char fp_head[FP_HEAD_CHARS + 1];
    memcpy(fp_head, DEMO_FP, FP_HEAD_CHARS);
    fp_head[FP_HEAD_CHARS] = '\0';

    CHECK(strstr(st, DEMO_FP) == NULL);
    CHECK(strstr(st, fp_head) == NULL);
    CHECK(strstr(st, "fp=") == NULL);
    printf("test: 'fp' appears in no form in the answer (memoryType 'sp' = obscured)\n");

    // Table 39: discovery is an action ('ac'), reporting where the binding maps
    // are. With the host wired in, that is the service module hosting one.
    id_service_set_inventory_source(so[0], fake_inventory, NULL);
    const char* disc = get_payload(so[2]);
    printf("test: GET %s -> %s\n", so[2]->path, disc);
    CHECK(strcmp(disc, "mt=sm,mi=1,ni=0,ad=") == 0);

    // ... and the counts (Table 42) come from the same hook.
    const char* st2 = get_payload(so[1]);
    CHECK(strstr(st2, "nh=2;nw=1;ns=1") != NULL);
    printf("test: inventory wired -> %s\n", st2);

    // Preset tables ('pr'/'ro'/'sp'/'po') are not writable over HES-CLME: the
    // PUT is ignored, and must not disturb the values.
    for (int i = 0; i < ID_SERVICE_OBJECT_COUNT; i++) {
        hes_clme_msg_t in = {0};
        in.verb = HES_VERB_PUT;
        hes_msg_set_path(&in, so[i]->path);
        hes_msg_set_payload_str(&in, "pi=0000");
        so[i]->on_put(so[i], &in);
    }
    CHECK(has_derived_pi(get_payload(so[1])));
    printf("test: PUT on each address ignored (every table is preset)\n");

    for (int i = 0; i < ID_SERVICE_OBJECT_COUNT; i++) {
        so[i]->destroy(so[i]);
        free(so[i]);
    }

    ////////////////////////////////////////////////////////////////////////////////
    // every way of provisioning it wrong fails closed
    ////////////////////////////////////////////////////////////////////////////////
    service_object_t* bad[ID_SERVICE_OBJECT_COUNT];
    CHECK(id_service_create(NULL, bad, ID_SERVICE_OBJECT_COUNT) == -1);
    CHECK(id_service_create("", bad, ID_SERVICE_OBJECT_COUNT) == -1);
    CHECK(id_service_create("no_such_identity.json", bad, ID_SERVICE_OBJECT_COUNT) == -1);
    CHECK(id_service_create("identity_bad_short.json", bad, ID_SERVICE_OBJECT_COUNT) == -1);
    CHECK(id_service_create("identity_bad_hex.json", bad, ID_SERVICE_OBJECT_COUNT) == -1);
    CHECK(id_service_create("identity_no_fp.json", bad, ID_SERVICE_OBJECT_COUNT) == -1);
    // 'pi' is optional, but a wrong one would advertise a key that does not
    // match 'fp' -- every challenge would fail with no visible reason -- so it
    // is a startup failure rather than a warning.
    CHECK(id_service_create("identity_wrong_pi.json", bad, ID_SERVICE_OBJECT_COUNT) == -1);
    CHECK(id_service_create("identity.json", bad, ID_SERVICE_OBJECT_COUNT - 1) == -1);
    printf("test: absent / empty / short / non-hex / fingerprint-less identities and a\n"
           "      too-small output array are all refused\n");

    printf("ALL TESTS PASSED\n");
    return 0;
}
