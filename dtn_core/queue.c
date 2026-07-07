/* dtn_core/queue.c — per-node bundle queue with class-aware admission.
 * Port of Project_DSN core/node.py + the Wave 2d preemption policy. */
#include "queue.h"
#include <string.h>

#define PREEMPTION_THRESHOLD_FRACTION 0.10f  /* <10% free -> pressure */

void queue_init(node_queue_t *q, node_id_t owner, float buf_cap, const char *policy)
{
    memset(q, 0, sizeof(*q));
    q->owner   = owner;
    q->buf_cap = buf_cap;
    strncpy(q->policy, policy ? policy : "accept", sizeof(q->policy) - 1);
}

float queue_load(const node_queue_t *q)
{
    float s = 0.0f;
    for (size_t i = 0; i < q->count; ++i) s += (float)q->slots[i].size;
    return s;
}

float queue_free(const node_queue_t *q)
{
    return q->buf_cap - queue_load(q);
}

int queue_can_accept(const node_queue_t *q, const bundle_t *b)
{
    return queue_free(q) >= (float)b->size;
}

static int find_lowest_class_victim(const node_queue_t *q, const bundle_t *incoming)
{
    /* Find the lowest-class bundle in the queue that is strictly lower
     * priority than the incoming one. Return its slot index, or -1. */
    int    idx   = -1;
    int8_t lowest = (int8_t)incoming->bundle_class;
    for (size_t i = 0; i < q->count; ++i) {
        int8_t c = (int8_t)q->slots[i].bundle_class;
        if (c < (int8_t)incoming->bundle_class && c < lowest) {
            lowest = c;
            idx    = (int)i;
        }
    }
    return idx;
}

int queue_enqueue(node_queue_t *q, const bundle_t *b)
{
    if (q->count >= NODE_QUEUE_MAX) return -1;

    if (queue_can_accept(q, b)) {
        q->slots[q->count++] = *b;
        return 0;
    }

    /* Buffer pressure: free < 10% of cap. Try preemption. */
    float free_bytes = queue_free(q);
    if (free_bytes >= PREEMPTION_THRESHOLD_FRACTION * q->buf_cap)
        return -1;   /* not under pressure, just full */

    int victim = find_lowest_class_victim(q, b);
    if (victim < 0) return -1;

    /* Evict the victim. */
    q->slots[victim] = q->slots[q->count - 1];
    --q->count;
    /* Now there is room (victim size >= incoming size? not necessarily,
     * but for MVP we assume the lowest-class bundle is at least as big;
     * if still not enough, reject). */
    if (queue_free(q) < (float)b->size) return -1;
    q->slots[q->count++] = *b;
    return 1;
}

int queue_dequeue(node_queue_t *q, uint64_t bundle_id)
{
    for (size_t i = 0; i < q->count; ++i) {
        if (q->slots[i].bundle_id == bundle_id) {
            q->slots[i] = q->slots[q->count - 1];
            --q->count;
            return 0;
        }
    }
    return -1;
}

static float rank(const bundle_t *b, dtn_time_t t)
{
    /* Default weights from Project_DSN. */
    return bundle_scheduling_key(b, t, 1.0f, 0.5f, 0.3f, 0.1f, 1e6f);
}

const bundle_t *queue_peek_next(const node_queue_t *q, dtn_time_t t)
{
    if (q->count == 0) return NULL;
    size_t best = 0;
    float  best_rank = rank(&q->slots[0], t);
    for (size_t i = 1; i < q->count; ++i) {
        float r = rank(&q->slots[i], t);
        if (r > best_rank) { best_rank = r; best = i; }
    }
    return &q->slots[best];
}

int queue_pop_next(node_queue_t *q, bundle_t *out, dtn_time_t t)
{
    if (q->count == 0) return -1;
    size_t best = 0;
    float  best_rank = rank(&q->slots[0], t);
    for (size_t i = 1; i < q->count; ++i) {
        float r = rank(&q->slots[i], t);
        if (r > best_rank) { best_rank = r; best = i; }
    }
    *out = q->slots[best];
    /* Swap-remove. */
    q->slots[best] = q->slots[q->count - 1];
    --q->count;
    return 0;
}

const bundle_t *queue_get(const node_queue_t *q, uint64_t bundle_id)
{
    for (size_t i = 0; i < q->count; ++i)
        if (q->slots[i].bundle_id == bundle_id)
            return &q->slots[i];
    return NULL;
}