/* dtn_core/trace.c — trace emission. Uses vsnprintf for the detail string,
 * which is available in both glibc and ESP-IDF's newlib. */
#include "trace.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

void tracer_init(tracer_t *tr, trace_sink_fn sink, void *user)
{
    tr->sink = sink;
    tr->user = user;
}

void tracer_emit(tracer_t *tr, dtn_time_t t, node_id_t node, trace_event_t ev,
                 uint64_t bundle_id, const char *detail_fmt, ...)
{
    if (!tr || !tr->sink) return;
    trace_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.t         = t;
    rec.node      = node;
    rec.event     = ev;
    rec.bundle_id = bundle_id;

    if (detail_fmt) {
        va_list ap;
        va_start(ap, detail_fmt);
        vsnprintf(rec.detail, sizeof(rec.detail), detail_fmt, ap);
        va_end(ap);
    }

    tr->sink(&rec, tr->user);
}

const char *trace_event_name(trace_event_t ev)
{
    switch (ev) {
    case TRACE_BUNDLE_GENERATED:   return "BUNDLE_GENERATED";
    case TRACE_BUNDLE_QUEUED:      return "BUNDLE_QUEUED";
    case TRACE_BUNDLE_ROUTING:      return "BUNDLE_ROUTING";
    case TRACE_BUNDLE_TX_START:    return "BUNDLE_TX_START";
    case TRACE_BUNDLE_TX_COMPLETE: return "BUNDLE_TX_COMPLETE";
    case TRACE_BUNDLE_ARRIVED:     return "BUNDLE_ARRIVED";
    case TRACE_CUSTODY_ACCEPTED:   return "CUSTODY_ACCEPTED";
    case TRACE_CUSTODY_RELEASED:   return "CUSTODY_RELEASED";
    case TRACE_CUSTODY_TIMEOUT:    return "CUSTODY_TIMEOUT";
    case TRACE_BUNDLE_DELIVERED:   return "BUNDLE_DELIVERED";
    case TRACE_BUNDLE_EXPIRED:      return "BUNDLE_EXPIRED";
    case TRACE_BUNDLE_DROPPED:      return "BUNDLE_DROPPED";
    case TRACE_CONTACT_OPEN:       return "CONTACT_OPEN";
    case TRACE_CONTACT_CLOSE:      return "CONTACT_CLOSE";
    case TRACE_DISRUPTION:         return "DISRUPTION";
    default: return "UNKNOWN";
    }
}