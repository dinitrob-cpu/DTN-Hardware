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
    dtn_time_t         time_offset;    /* offset added to local clock to sync to ground station epoch */
    int                time_synced;   /* 1 once a time-sync message has been received */
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
 * or by an injected message on any node). `t` is the current DTN time
 * (from the platform's wall clock + time_offset). Returns the bundle_id,
 * or 0 on failure. */
uint64_t node_generate_bundle(node_state_t *ns, node_id_t dst,
                              bundle_class_t cls, const char *msg,
                              dtn_time_t t);

/* --- Time sync ---
 * The ground station (Pi) periodically broadcasts its current DTN time
 * over LoRa. ESP32 nodes receive it and set their time_offset so their
 * local clock aligns to the ground station's epoch. This lets all nodes
 * agree on when scheduled contacts open/close without needing NTP or an RTC.
 *
 * Wire format: 2-byte magic 0x5453 ("TS") + 4-byte float (ground station's
 * DTN time at the moment of transmission). 6 bytes total — fits in a single
 * LoRa frame trivially. */
#define TIME_SYNC_MAGIC 0x5453u

/* Called by the ground station to broadcast a time-sync message.
 * Returns 0 on success, -1 on TX failure. */
int node_send_time_sync(node_state_t *ns, dtn_time_t t);

/* Called by any node when a LoRa frame arrives. If the frame is a
 * time-sync message, sets ns->time_offset so that the platform's local
 * clock + time_offset == ground station epoch. Returns 1 if the frame
 * was a time-sync (handled), 0 if it wasn't a time-sync frame (caller
 * should try bundle_deserialize), -1 on error. */
int node_try_handle_time_sync(node_state_t *ns, dtn_time_t local_t,
                               const uint8_t *frame, size_t len);

/* Main per-tick step. Called by the platform event loop at each iteration.
 * Checks contact windows, custody timeouts, dequeues + routes + transmits.
 * Does NOT call lora_recv() — receive is driven by the platform via
 * node_try_recv() when the LoRa IRQ fd fires.
 * Returns 0 on success, -1 on a non-fatal error. */
int node_tick(node_state_t *ns, dtn_time_t t);

/* Called by the platform event loop when the LoRa IRQ fd becomes readable
 * (DIO0 rising edge = RX_DONE). Reads the frame and hands it to the
 * forwarding engine. Returns 0 on success, 1 if no frame (spurious), -1 on error. */
int node_try_recv(node_state_t *ns, dtn_time_t t);

/* Called when a LoRa frame arrives. Deserializes the bundle, accepts
 * custody if appropriate, enqueues for further forwarding, and emits
 * trace events. Returns 0 on success. */
int node_handle_received_frame(node_state_t *ns, dtn_time_t t,
                                const uint8_t *frame, size_t len);

#endif /* NODE_FORWARDING_H */