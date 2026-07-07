#ifndef DTN_CORE_CUSTODY_H
#define DTN_CORE_CUSTODY_H

#include "bundle.h"
#include "contact.h"
#include "types.h"

#define CUSTODY_TIMEOUT_DEFAULT 1800.0f   /* 30 minutes, like Project_DSN */
#define CUSTODY_MAX_RECORDS 32

typedef struct {
    uint64_t   bundle_id;
    node_id_t   custodian;
    dtn_time_t  t_accept;
    dtn_time_t  timeout;       /* absolute time at which the timer fires */
} custody_record_t;

typedef struct {
    custody_record_t records[CUSTODY_MAX_RECORDS];
    size_t           count;
    dtn_time_t       default_timeout;
} custody_manager_t;

void custody_init(custody_manager_t *cm, dtn_time_t default_timeout);

/* Acceptance decision (RFC 5050-style, simplified). */
int  custody_can_accept(const custody_manager_t *cm, const bundle_t *b,
                        node_id_t node_id, dtn_time_t t,
                        float buf_free, const char *policy);

/* Record a custody transfer. Returns pointer to the record (or NULL if full). */
const custody_record_t *custody_transfer(custody_manager_t *cm, bundle_t *b,
                                         const contact_t *contact, dtn_time_t t);

void custody_release (custody_manager_t *cm, uint64_t bundle_id);
int  custody_is_timed_out(const custody_manager_t *cm, uint64_t bundle_id, dtn_time_t t);

const custody_record_t *custody_get(const custody_manager_t *cm, uint64_t bundle_id);

#endif /* DTN_CORE_CUSTODY_H */