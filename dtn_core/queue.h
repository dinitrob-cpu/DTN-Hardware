#ifndef DTN_CORE_QUEUE_H
#define DTN_CORE_QUEUE_H

#include "bundle.h"
#include "types.h"

#define NODE_QUEUE_MAX 16   /* per-node buffer slots; enough for MVP */

/* Per-node queue with class-aware admission + preemption.
 * Port of Project_DSN's core/node.py + the Wave 2d policy upgrade. */
typedef struct {
    bundle_t   slots[NODE_QUEUE_MAX];
    size_t     count;
    float      buf_cap;        /* bytes */
    node_id_t  owner;
    char       policy[8];      /* "accept" (default) or "reject" */
} node_queue_t;

void queue_init(node_queue_t *q, node_id_t owner, float buf_cap, const char *policy);

float queue_load(const node_queue_t *q);     /* sum of bundle sizes */
float queue_free(const node_queue_t *q);

int   queue_can_accept(const node_queue_t *q, const bundle_t *b);

/* Enqueue with class-aware admission. Under buffer pressure (<10% free),
 * higher-class bundles preempt lower-class queued bundles. Returns:
 *   0  -> enqueued normally
 *   1  -> enqueued after preempting lower-class bundle(s)
 *  -1  -> rejected (no room, no preemption candidates) */
int   queue_enqueue(node_queue_t *q, const bundle_t *b);

/* Remove a bundle from the queue by id. Returns 0 on success, -1 if absent. */
int   queue_dequeue(node_queue_t *q, uint64_t bundle_id);

/* Peek the highest-rank bundle at time t (does not remove). Returns NULL
 * if the queue is empty. Rank uses bundle_scheduling_key with default weights. */
const bundle_t *queue_peek_next(const node_queue_t *q, dtn_time_t t);

/* Pop the highest-rank bundle (copies it into *out and removes from queue).
 * Returns 0 on success, -1 if empty. */
int   queue_pop_next(node_queue_t *q, bundle_t *out, dtn_time_t t);

const bundle_t *queue_get(const node_queue_t *q, uint64_t bundle_id);

#endif /* DTN_CORE_QUEUE_H */