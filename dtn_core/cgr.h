#ifndef DTN_CORE_CGR_H
#define DTN_CORE_CGR_H

#include "bundle.h"
#include "contact.h"
#include "types.h"
#include <stddef.h>

/* CGR configuration. Defaults mirror Project_DSN's CGREngine defaults. */
typedef struct {
    int   max_hops;     /* default 10 (BUNDLE_MAX_HOPS) */
    int   max_routes;   /* K-best beam width: 16 on ESP32, 64 on Pi */
    float w1;           /* delay weight      default 1.0 */
    float w2;           /* confidence weight default 0.5 */
    float w3;           /* capacity-margin   default 0.3 */
    int   ber_aware;    /* 1 -> feas_real gates on (1-ber)^(8*size) >= 0.5 */
} cgr_config_t;

void cgr_config_default(cgr_config_t *cfg, int max_routes);

/* Selected route — a list of contact pointers. The contacts must remain
 * valid in the contact_plan for the lifetime of the route (they do — the
 * plan is loaded once at boot). */
#define CGR_ROUTE_MAX_HOPS 10

typedef struct {
    const contact_t *hops[CGR_ROUTE_MAX_HOPS];
    size_t          hop_count;
    float           cost;          /* J(alpha, rho, t) */
    dtn_time_t      arrival_time;  /* A_m at the destination */
} cgr_route_t;

/* Select the best feasible route for bundle `b` at time `t`.
 * Returns 0 on success (route_out populated), -1 if no route exists. */
int cgr_select_route(const contact_plan_t *cp, const bundle_t *b,
                     dtn_time_t t, const cgr_config_t *cfg,
                     cgr_route_t *route_out);

/* BER-aware feasibility: returns 1 if (1-ber)^(8*size) >= 0.5. */
int cgr_contact_ber_feasible(const contact_t *c, float bundle_size_bytes);

#endif /* DTN_CORE_CGR_H */