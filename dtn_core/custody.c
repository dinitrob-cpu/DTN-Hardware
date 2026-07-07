/* dtn_core/custody.c — custody transfer manager. Port of Project_DSN's
 * protocol/custody.py. */
#include "custody.h"
#include <string.h>

void custody_init(custody_manager_t *cm, dtn_time_t default_timeout)
{
    memset(cm, 0, sizeof(*cm));
    cm->default_timeout = default_timeout > 0.0f ? default_timeout : CUSTODY_TIMEOUT_DEFAULT;
}

int custody_can_accept(const custody_manager_t *cm, const bundle_t *b,
                       node_id_t node_id, dtn_time_t t,
                       float buf_free, const char *policy)
{
    if (!bundle_is_alive(b, t))           return 0;
    if (buf_free < (float)b->size)        return 0;
    if (policy && policy[0] == 'r' && policy[1] == 'e' && policy[2] == 'j')
        return 0;   /* "reject" */
    /* No re-custody to the same custodian. */
    for (size_t i = 0; i < cm->count; ++i) {
        if (cm->records[i].bundle_id == b->bundle_id &&
            cm->records[i].custodian == node_id)
            return 0;
    }
    return 1;
}

const custody_record_t *custody_transfer(custody_manager_t *cm, bundle_t *b,
                                          const contact_t *contact, dtn_time_t t)
{
    /* Update existing record in place if present, else append. */
    for (size_t i = 0; i < cm->count; ++i) {
        if (cm->records[i].bundle_id == b->bundle_id) {
            cm->records[i].custodian = b->curr;
            cm->records[i].t_accept   = t;
            cm->records[i].timeout    = t + cm->default_timeout;
            b->cust = b->curr;
            return &cm->records[i];
        }
    }
    if (cm->count >= CUSTODY_MAX_RECORDS) return NULL;
    custody_record_t *r = &cm->records[cm->count++];
    r->bundle_id = b->bundle_id;
    r->custodian = b->curr;
    r->t_accept  = t;
    r->timeout   = t + cm->default_timeout;
    b->cust      = b->curr;
    return r;
}

void custody_release(custody_manager_t *cm, uint64_t bundle_id)
{
    for (size_t i = 0; i < cm->count; ++i) {
        if (cm->records[i].bundle_id == bundle_id) {
            /* Swap-remove. */
            cm->records[i] = cm->records[cm->count - 1];
            --cm->count;
            return;
        }
    }
}

int custody_is_timed_out(const custody_manager_t *cm, uint64_t bundle_id, dtn_time_t t)
{
    for (size_t i = 0; i < cm->count; ++i) {
        if (cm->records[i].bundle_id == bundle_id)
            return t >= cm->records[i].timeout;
    }
    return 0;
}

const custody_record_t *custody_get(const custody_manager_t *cm, uint64_t bundle_id)
{
    for (size_t i = 0; i < cm->count; ++i)
        if (cm->records[i].bundle_id == bundle_id)
            return &cm->records[i];
    return NULL;
}