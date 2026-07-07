#ifndef DTN_CORE_DISRUPTION_H
#define DTN_CORE_DISRUPTION_H

#include "types.h"

/* Disruption model: probabilistic link failure + exponential backoff.
 * Port of Project_DSN's disruption/DisruptionModel.py.
 * Each node owns one of these per neighbor link. */
typedef struct {
    node_id_t   peer;          /* the neighbor this model applies to */
    float       p_fail;        /* per-window probability of a failure */
    float       mean_duration; /* seconds, exponential distribution */
    dtn_time_t  backoff_until;  /* time until which the link is forced down */
    int         failed;         /* currently in a failure episode */
    dtn_time_t  fail_started;  /* start time of the current episode */
    dtn_time_t  fail_until;     /* scheduled end time of the current episode */
} disruption_model_t;

void disruption_init(disruption_model_t *dm, node_id_t peer,
                     float p_fail, float mean_duration);

/* Probe at time t. Updates internal state. Returns 1 if the link is
 * currently disrupted, 0 if it is usable. */
int  disruption_is_down(disruption_model_t *dm, dtn_time_t t);

/* Force a backoff window of `seconds` starting at t (e.g. after a TX
 * failure). */
void disruption_backoff(disruption_model_t *dm, dtn_time_t t, float seconds);

#endif /* DTN_CORE_DISRUPTION_H */