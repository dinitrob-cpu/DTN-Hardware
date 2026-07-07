#ifndef DTN_CORE_TRACE_H
#define DTN_CORE_TRACE_H

#include "bundle.h"
#include "types.h"

typedef enum {
    TRACE_BUNDLE_GENERATED   = 0,
    TRACE_BUNDLE_QUEUED,
    TRACE_BUNDLE_ROUTING,
    TRACE_BUNDLE_TX_START,
    TRACE_BUNDLE_TX_COMPLETE,
    TRACE_BUNDLE_ARRIVED,
    TRACE_CUSTODY_ACCEPTED,
    TRACE_CUSTODY_RELEASED,
    TRACE_CUSTODY_TIMEOUT,
    TRACE_BUNDLE_DELIVERED,
    TRACE_BUNDLE_EXPIRED,
    TRACE_BUNDLE_DROPPED,
    TRACE_CONTACT_OPEN,
    TRACE_CONTACT_CLOSE,
    TRACE_DISRUPTION
} trace_event_t;

typedef struct {
    dtn_time_t     t;
    node_id_t      node;
    trace_event_t  event;
    uint64_t       bundle_id;
    char           detail[64];   /* free-form short text, NUL-terminated */
} trace_record_t;

/* Sink callback. Each platform installs one (stdout on Pi, Serial on ESP32). */
typedef void (*trace_sink_fn)(const trace_record_t *rec, void *user);

typedef struct {
    trace_sink_fn sink;
    void         *user;
} tracer_t;

void tracer_init(tracer_t *tr, trace_sink_fn sink, void *user);
void tracer_emit (tracer_t *tr, dtn_time_t t, node_id_t node, trace_event_t ev,
                  uint64_t bundle_id, const char *detail_fmt, ...);

const char *trace_event_name(trace_event_t ev);

#endif /* DTN_CORE_TRACE_H */