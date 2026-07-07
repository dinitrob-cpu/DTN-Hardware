#ifndef DTN_CORE_CONTACT_H
#define DTN_CORE_CONTACT_H

#include "types.h"
#include <stddef.h>

typedef enum {
    CONTACT_UP        = 0,
    CONTACT_DOWN      = 1,
    CONTACT_UNCERTAIN  = 2
} contact_status_t;

/* Contact descriptor beta(c) — port of Project_DSN's Contact. */
typedef struct {
    node_id_t       frm;        /* transmitting node */
    node_id_t       to;         /* receiving node */
    dtn_time_t      t_start;
    dtn_time_t      t_end;
    float           rate;       /* bits/s (keep consistent with bundle size in bytes) */
    dtn_time_t      owlt;      /* one-way light time (seconds) */
    float           conf;      /* confidence in [0,1] */
    float           ber;       /* bit-error rate */
    contact_status_t status;

    /* runtime accounting */
    float           bytes_used;   /* bytes consumed so far */
    float           reserved;     /* bytes reserved by routes not yet sent */
} contact_t;

#define CONTACT_MODE_SCHEDULED 0  /* informational; all MVP contacts are scheduled */

/* Predicates. */
int contact_is_active(const contact_t *c, dtn_time_t t); /* t_start <= t < t_end and UP */
int contact_is_future(const contact_t *c, dtn_time_t t); /* t < t_start and not DOWN */

/* Capacity helpers. */
float contact_duration(const contact_t *c);
float contact_capacity(const contact_t *c);     /* rate * duration */
float contact_cap_rem(const contact_t *c, dtn_time_t t);
float contact_cap_use(const contact_t *c, dtn_time_t t);
void  contact_reserve (contact_t *c, float size);
void  contact_release_reservation(contact_t *c, float size);
void  contact_consume (contact_t *c, float size);

/* Bundle eligibility: active and has residual capacity. */
int   contact_is_feasible_for(const contact_t *c, float bundle_size, dtn_time_t t);

/* Transmission timing (Phase 1.2 Section 4). */
dtn_time_t contact_tx_duration(const contact_t *c, float bundle_size);
dtn_time_t contact_earliest_start(const contact_t *c, dtn_time_t arrival_time);
dtn_time_t contact_arrival_at_receiver(const contact_t *c, float bundle_size, dtn_time_t start_time);

/* --- Contact plan --- */

#define CONTACT_PLAN_MAX 64   /* enough for the MVP topology + a few extras */

typedef struct {
    contact_t contacts[CONTACT_PLAN_MAX];
    size_t     count;
    /* Index by source node for O(degree) lookup. Built lazily.
     * For simplicity we scan on lookups; with CONTACT_PLAN_MAX=64 this is fine.
     * (Project_DSN builds an explicit dict; we keep it flat for the MVP.) */
} contact_plan_t;

int  contact_plan_load_json(contact_plan_t *cp, const char *json_path);
void contact_plan_add(contact_plan_t *cp, const contact_t *c);

/* Lookups. */
const contact_t *contact_plan_get(const contact_plan_t *cp, node_id_t frm, node_id_t to);
size_t contact_plan_from_node(const contact_plan_t *cp, node_id_t frm,
                              const contact_t **out, size_t out_cap);

#endif /* DTN_CORE_CONTACT_H */