/* node/forwarding.c — the DTN forwarding engine, shared by both node apps.
 *
 * This ties together the contact plan, queue, CGR router, custody manager,
 * and LoRa transport. The platform-specific main (main_pi.c / main_esp32.c)
 * owns the event loop and calls node_tick() at each iteration, plus
 * node_handle_received_frame() when a LoRa frame arrives.
 */
#include "forwarding.h"
#include "../dtn_core/bundle.h"
#include "../dtn_core/contact.h"
#include "../dtn_core/cgr.h"
#include "../dtn_core/custody.h"
#include "../dtn_core/queue.h"
#include "../dtn_core/trace.h"
#include <string.h>
#include <stdio.h>

static dtn_time_t now_seconds(dtn_time_t boot_time)
{
    /* Platform code should pass a real clock; this is a fallback that
     * returns 0. The Pi and ESP32 mains inject real time. */
    (void)boot_time;
    return 0.0f;
}

void node_state_init(node_state_t *ns, node_id_t self, float buf_cap,
                     const char *policy, lora_transport_t *radio,
                     trace_sink_fn sink, void *sink_user)
{
    memset(ns, 0, sizeof(*ns));
    ns->self_id = self;
    queue_init(&ns->queue, self, buf_cap, policy);
    custody_init(&ns->custody, CUSTODY_TIMEOUT_DEFAULT);
    cgr_config_default(&ns->cgr_cfg, CGR_MAX_ROUTES_ESP);  /* conservative */
    tracer_init(&ns->tracer, sink, sink_user);
    ns->radio = radio;
}

uint64_t node_generate_bundle(node_state_t *ns, node_id_t dst,
                              bundle_class_t cls, const char *msg)
{
    bundle_t b;
    memset(&b, 0, sizeof(b));
    /* Bundle id: combine self_id + a counter. For MVP use a static counter. */
    static uint64_t counter = 1;
    b.bundle_id    = ((uint64_t)ns->self_id << 32) | (counter++);
    b.src          = ns->self_id;
    b.dst          = dst;
    b.curr         = ns->self_id;
    b.cust         = ns->self_id;
    b.prio         = (uint8_t)cls;
    b.bundle_class = cls;
    b.t_gen        = now_seconds(ns->boot_time);
    b.ttl          = 3600.0f;   /* 1 hour */
    size_t mlen = strlen(msg);
    if (mlen > BUNDLE_MAX_PAYLOAD) mlen = BUNDLE_MAX_PAYLOAD;
    memcpy(b.payload, msg, mlen);
    b.payload_len  = (uint16_t)mlen;
    b.size         = (uint16_t)(41 + mlen + 4);  /* wire size */

    if (queue_enqueue(&ns->queue, &b) < 0) return 0;
    char dst_str[5]; node_id_to_str(dst, dst_str);
    tracer_emit(&ns->tracer, b.t_gen, ns->self_id,
                TRACE_BUNDLE_GENERATED, b.bundle_id,
                "dst=%s msg_len=%u", dst_str, (unsigned)mlen);
    return b.bundle_id;
}

int node_handle_received_frame(node_state_t *ns, dtn_time_t t,
                               const uint8_t *frame, size_t len)
{
    bundle_t b;
    if (bundle_deserialize(&b, frame, len) != 0) {
        tracer_emit(&ns->tracer, t, ns->self_id,
                    TRACE_BUNDLE_DROPPED, 0, "bad_frame len=%u", (unsigned)len);
        return -1;
    }
    ns->bundles_rx++;
    ns->last_rssi = lora_rssi(ns->radio);

    /* If this node is the destination -> delivered. */
    if (b.dst == ns->self_id) {
        b.status = BUNDLE_DELIVERED;
        ns->bundles_delivered++;
        tracer_emit(&ns->tracer, t, ns->self_id,
                    TRACE_BUNDLE_DELIVERED, b.bundle_id,
                    "from=%u msg=%.*s", b.src, b.payload_len, b.payload);
        /* If we held custody, release it. */
        custody_release(&ns->custody, b.bundle_id);
        return 0;
    }

    /* Otherwise accept custody and enqueue for forwarding. */
    if (custody_can_accept(&ns->custody, &b, ns->self_id, t,
                            queue_free(&ns->queue), ns->queue.policy)) {
        b.curr = ns->self_id;
        const contact_t *c = contact_plan_get(&ns->plan, b.src, ns->self_id);
        custody_transfer(&ns->custody, &b, c, t);
        tracer_emit(&ns->tracer, t, ns->self_id,
                     TRACE_CUSTODY_ACCEPTED, b.bundle_id,
                     "from=%u", b.src);
        queue_enqueue(&ns->queue, &b);
        tracer_emit(&ns->tracer, t, ns->self_id,
                     TRACE_BUNDLE_QUEUED, b.bundle_id,
                     "queue_len=%u", (unsigned)ns->queue.count);
    } else {
        tracer_emit(&ns->tracer, t, ns->self_id,
                     TRACE_BUNDLE_DROPPED, b.bundle_id, "custody_rejected");
    }
    return 0;
}

/* Try to send one bundle over an active contact. */
static int try_send_one(node_state_t *ns, dtn_time_t t)
{
    bundle_t b;
    if (queue_pop_next(&ns->queue, &b, t) != 0) return 0;   /* empty */
    if (!bundle_is_alive(&b, t)) {
        tracer_emit(&ns->tracer, t, ns->self_id,
                     TRACE_BUNDLE_EXPIRED, b.bundle_id, "ttl_exceeded");
        custody_release(&ns->custody, b.bundle_id);
        return 0;
    }

    /* Route via CGR. */
    cgr_route_t route;
    if (cgr_select_route(&ns->plan, &b, t, &ns->cgr_cfg, &route) != 0) {
        /* No route right now — re-queue and wait. */
        queue_enqueue(&ns->queue, &b);
        return 0;
    }
    /* First hop determines the next contact. */
    const contact_t *first = route.hops[0];
    if (!contact_is_feasible_for(first, (float)b.size, t)) {
        queue_enqueue(&ns->queue, &b);
        return 0;
    }

    /* Serialize + transmit. */
    uint8_t frame[256];
    int n = bundle_serialize(&b, frame, sizeof(frame));
    if (n < 0) { queue_enqueue(&ns->queue, &b); return 0; }

    int sent = lora_send(ns->radio, frame, (size_t)n);
    if (sent > 0) {
        contact_consume((contact_t *)first, (float)b.size);
        ns->bundles_tx++;
        b.status = BUNDLE_IN_TRANSIT;
        tracer_emit(&ns->tracer, t, ns->self_id,
                     TRACE_BUNDLE_TX_START, b.bundle_id,
                     "next_hop=%u via_contact=%u->%u",
                     first->to, first->frm, first->to);
    } else {
        /* TX failed; re-queue for retry. */
        queue_enqueue(&ns->queue, &b);
        tracer_emit(&ns->tracer, t, ns->self_id,
                     TRACE_BUNDLE_DROPPED, b.bundle_id, "tx_failed");
    }
    return 1;
}

int node_tick(node_state_t *ns, dtn_time_t t)
{
    /* 1. Custody timeouts. */
    for (size_t i = 0; i < ns->custody.count; ) {
        if (custody_is_timed_out(&ns->custody, ns->custody.records[i].bundle_id, t)) {
            tracer_emit(&ns->tracer, t, ns->self_id,
                         TRACE_CUSTODY_TIMEOUT, ns->custody.records[i].bundle_id,
                         "custodian=%u", ns->custody.records[i].custodian);
            custody_release(&ns->custody, ns->custody.records[i].bundle_id);
            /* The bundle is still in our queue (we held custody); CGR will
             * re-route it on the next tick. */
        } else {
            ++i;
        }
    }

    /* 2. Try to send one bundle (the queue pop selects the highest-rank one). */
    try_send_one(ns, t);

    /* 3. Try to receive (with a short timeout). The platform main may
     * instead drive receive via an IRQ; this is the fallback polling path. */
    uint8_t rbuf[256];
    int r = lora_recv(ns->radio, rbuf, sizeof(rbuf), 100);
    if (r > 0) node_handle_received_frame(ns, t, rbuf, (size_t)r);

    return 0;
}