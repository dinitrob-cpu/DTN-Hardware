#ifndef NODE_FORWARDING_H
#define NODE_FORWARDING_H

#include "../dtn_core/bundle.h"
#include "../dtn_core/contact.h"
#include "../dtn_core/cgr.h"
#include "../dtn_core/custody.h"
#include "../dtn_core/queue.h"
#include "../dtn_core/trace.h"
#include "../dtn_core/types.h"
#include "../transport/lora_transport.h"

/* Per-node runtime state. Shared between the Pi and ESP32 node apps.
 * Each app owns one of these and drives it from its own event loop. */
typedef struct {
    node_id_t          self_id;
    contact_plan_t     plan;
    node_queue_t       queue;
    custody_manager_t  custody;
    cgr_config_t       cgr_cfg;
    tracer_t            tracer;
    lora_transport_t  *radio;
    dtn_time_t         boot_time;     /* wall-clock seconds at boot */
    /* Stats for the status command / dashboard. */
    uint64_t           bundles_tx;
    uint64_t           bundles_rx;
    uint64_t           bundles_delivered;
    int                last_rssi;
} node_state_t;

void node_state_init(node_state_t *ns, node_id_t self, float buf_cap,
                     const char *policy, lora_transport_t *radio,
                     trace_sink_fn sink, void *sink_user);

/* Build a new bundle (called by the CLI / dashboard on the ground station,
 * or by an injected message on any node). Returns the bundle_id assigned. */
uint64_t node_generate_bundle(node_state_t *ns, node_id_t dst,
                              bundle_class_t cls, const char *msg);

/* Main per-tick step. Called by the platform event loop at each iteration.
 * Checks contact windows, custody timeouts, dequeues + routes + transmits.
 * Returns 0 on success, -1 on a non-fatal error. */
int node_tick(node_state_t *ns, dtn_time_t t);

/* Called when a LoRa frame arrives. Deserializes the bundle, accepts
 * custody if appropriate, enqueues for further forwarding, and emits
 * trace events. Returns 0 on success. */
int node_handle_received_frame(node_state_t *ns, dtn_time_t t,
                                const uint8_t *frame, size_t len);

#endif /* NODE_FORWARDING_H */