/* tests/test_core.c — sanity tests for the dtn_core library.
 *
 * Plain asserts, run on the Pi. Verifies:
 *   - node_id round-trips
 *   - bundle serialize/deserialize round-trips and CRC catches corruption
 *   - contact plan loads from JSON
 *   - CGR finds a route on a tiny plan
 *   - custody transfer + timeout
 *   - queue enqueue + class-aware preemption
 */
#include "../dtn_core/types.h"
#include "../dtn_core/bundle.h"
#include "../dtn_core/contact.h"
#include "../dtn_core/cgr.h"
#include "../dtn_core/custody.h"
#include "../dtn_core/queue.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

static void test_node_id(void)
{
    char s[5];
    node_id_t id = node_id_from_str("EART");
    node_id_to_str(id, s);
    CHECK(strcmp(s, "EART") == 0, "node_id round-trip EART");
    id = node_id_from_str("ROVR");
    node_id_to_str(id, s);
    CHECK(strcmp(s, "ROVR") == 0, "node_id round-trip ROVR");
}

static void test_bundle_serdes(void)
{
    bundle_t b;
    memset(&b, 0, sizeof(b));
    b.bundle_id    = 0x1122334455667788ULL;
    b.src          = node_id_from_str("EART");
    b.dst          = node_id_from_str("ROVR");
    b.curr         = node_id_from_str("EART");
    b.cust         = b.curr;
    b.prio         = 2;
    b.bundle_class = BUNDLE_CLASS_SCIENCE;
    b.t_gen        = 0.0f;
    b.ttl          = 3600.0f;
    const char *msg = "hello mars";
    size_t mlen = strlen(msg);
    memcpy(b.payload, msg, mlen);
    b.payload_len = (uint16_t)mlen;
    b.size        = (uint16_t)(41 + mlen + 4);

    uint8_t buf[256];
    int n = bundle_serialize(&b, buf, sizeof(buf));
    CHECK(n > 0, "serialize ok");

    bundle_t b2;
    int r = bundle_deserialize(&b2, buf, (size_t)n);
    CHECK(r == 0, "deserialize ok");
    CHECK(b2.bundle_id == b.bundle_id, "bundle_id preserved");
    CHECK(b2.src == b.src, "src preserved");
    CHECK(b2.dst == b.dst, "dst preserved");
    CHECK(b2.payload_len == b.payload_len, "payload_len preserved");
    CHECK(memcmp(b2.payload, b.payload, mlen) == 0, "payload preserved");

    /* Corrupt one byte and expect deserialization to fail. */
    buf[10] ^= 0xFF;
    r = bundle_deserialize(&b2, buf, (size_t)n);
    CHECK(r != 0, "CRC catches corruption");
}

static void test_contact_plan(void)
{
    contact_plan_t cp;
    int n = contact_plan_load_json(&cp, "config/contact_plan.json");
    CHECK(n > 0, "contact plan loaded");
    const contact_t *c = contact_plan_get(&cp, node_id_from_str("EART"), node_id_from_str("ERLY"));
    CHECK(c != NULL, "found EART->ERLY contact");
    CHECK(c->rate == 1200.0f, "rate correct");
}

static void test_cgr(void)
{
    /* Build a tiny plan: A -> B -> C. */
    contact_plan_t cp; cp.count = 0;
    contact_t ab = { node_id_from_str("AAAA"), node_id_from_str("BBBB"),
                     0.0f, 60.0f, 1000.0f, 0.0f, 1.0f, 0.0f, CONTACT_UP, 0, 0 };
    contact_t bc = { node_id_from_str("BBBB"), node_id_from_str("CCCC"),
                     0.0f, 60.0f, 1000.0f, 0.0f, 1.0f, 0.0f, CONTACT_UP, 0, 0 };
    contact_plan_add(&cp, &ab);
    contact_plan_add(&cp, &bc);

    bundle_t b; memset(&b, 0, sizeof(b));
    b.bundle_id = 1;
    b.src = node_id_from_str("AAAA");
    b.dst = node_id_from_str("CCCC");
    b.curr = node_id_from_str("AAAA");
    b.size = 100;
    b.ttl = 1000.0f;

    cgr_config_t cfg; cgr_config_default(&cfg, 16);
    cgr_route_t route;
    int r = cgr_select_route(&cp, &b, 1.0f, &cfg, &route);
    CHECK(r == 0, "CGR found a route");
    CHECK(route.hop_count == 2, "route has 2 hops");
    CHECK(route.hops[0]->to == node_id_from_str("BBBB"), "first hop A->B");
    CHECK(route.hops[1]->to == node_id_from_str("CCCC"), "second hop B->C");
}

static void test_custody(void)
{
    custody_manager_t cm; custody_init(&cm, 10.0f);
    bundle_t b; memset(&b, 0, sizeof(b));
    b.bundle_id = 42; b.ttl = 1000.0f; b.size = 10;
    b.curr = node_id_from_str("BBBB");
    int ok = custody_can_accept(&cm, &b, b.curr, 0.0f, 1000.0f, "accept");
    CHECK(ok, "can_accept ok");
    const custody_record_t *rec = custody_transfer(&cm, &b, NULL, 0.0f);
    CHECK(rec != NULL, "transfer ok");
    CHECK(b.cust == b.curr, "cust field set");
    CHECK(!custody_is_timed_out(&cm, 42, 5.0f), "not timed out at t=5");
    CHECK(custody_is_timed_out(&cm, 42, 11.0f), "timed out at t=11");
    custody_release(&cm, 42);
    CHECK(cm.count == 0, "release cleared record");
}

static void test_queue_preemption(void)
{
    node_queue_t q;
    queue_init(&q, node_id_from_str("AAAA"), 100.0f, "accept");
    /* Fill with housekeeping bundles near capacity. */
    bundle_t hk; memset(&hk, 0, sizeof(hk));
    hk.bundle_class = BUNDLE_CLASS_HOUSEKEEPING; hk.size = 45;
    hk.bundle_id = 1; hk.ttl = 1000.0f; hk.prio = 0;
    CHECK(queue_enqueue(&q, &hk) == 0, "enqueue hk 1");
    hk.bundle_id = 2;
    CHECK(queue_enqueue(&q, &hk) == 0, "enqueue hk 2");  /* 90/100 used */

    bundle_t cmd; memset(&cmd, 0, sizeof(cmd));
    cmd.bundle_class = BUNDLE_CLASS_COMMAND; cmd.size = 30;
    cmd.bundle_id = 3; cmd.ttl = 1000.0f; cmd.prio = 3;
    int r = queue_enqueue(&q, &cmd);
    CHECK(r == 1, "command preempted housekeeping");
    /* One of the two hk bundles should have been evicted. */
    CHECK(q.count == 2, "queue has 2 bundles after preemption");
}

int main(void)
{
    test_node_id();
    test_bundle_serdes();
    test_contact_plan();
    test_cgr();
    test_custody();
    test_queue_preemption();
    if (failures == 0) {
        printf("ALL TESTS PASSED\n");
        return 0;
    }
    printf("%d FAILURES\n", failures);
    return 1;
}