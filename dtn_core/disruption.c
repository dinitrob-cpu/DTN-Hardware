/* dtn_core/disruption.c — probabilistic link failure + backoff.
 * Port of Project_DSN disruption/DisruptionModel.py. Uses a simple
 * linear congruential generator so behavior is deterministic on both
 * Pi and ESP32 (newlib's rand() would differ; we keep our own state). */
#include "disruption.h"
#include <math.h>
#include <string.h>

static uint32_t lcg_next(uint32_t *state)
{
    /* Numerical Recipes LCG. */
    *state = (*state * 1664525u + 1013904223u);
    return *state;
}

static float lcg_uniform(uint32_t *state)
{
    return (float)((lcg_next(state) >> 8) & 0xFFFFFF) / (float)0x1000000;
}

void disruption_init(disruption_model_t *dm, node_id_t peer,
                    float p_fail, float mean_duration)
{
    memset(dm, 0, sizeof(*dm));
    dm->peer          = peer;
    dm->p_fail        = p_fail;
    dm->mean_duration = mean_duration;
}

int disruption_is_down(disruption_model_t *dm, dtn_time_t t)
{
    /* If inside a backoff window, still down. */
    if (t < dm->backoff_until) return 1;

    if (dm->failed) {
        if (t >= dm->fail_until) {
            /* Episode ended. */
            dm->failed = 0;
        } else {
            return 1;
        }
    }

    /* Roll for a new failure. Use a per-model LCG seeded from the peer id
     * and the current second so it's pseudo-random but reproducible. */
    static uint32_t rng_state = 0;
    if (rng_state == 0) rng_state = 0x12345678u ^ (uint32_t)dm->peer;
    uint32_t st = rng_state ^ (uint32_t)(t * 1000.0f);
    if (lcg_uniform(&st) < dm->p_fail) {
        /* Start an episode with exponential duration. */
        float u = lcg_uniform(&st);
        if (u < 1e-3f) u = 1e-3f;
        float dur = -logf(u) * dm->mean_duration;
        dm->failed       = 1;
        dm->fail_started = t;
        dm->fail_until   = t + (dtn_time_t)dur;
        rng_state = st;
        return 1;
    }
    rng_state = st;
    return 0;
}

void disruption_backoff(disruption_model_t *dm, dtn_time_t t, float seconds)
{
    dtn_time_t until = t + (dtn_time_t)seconds;
    if (until > dm->backoff_until) dm->backoff_until = until;
}