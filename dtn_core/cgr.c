/* dtn_core/cgr.c — Contact Graph Routing engine.
 * Iterative-DFS port of Project_DSN's routing/cgr.py, with the three C1
 * prunings: node-visited set, bounded Pareto dominance frontier per node,
 * K-best beam. No recursion (ESP32 stack is small); DFS state is kept on
 * a fixed-size stack array. BER-aware feasibility per Wave 2b. */
#include "cgr.h"
#include "contact.h"
#include "types.h"
#include <math.h>
#include <string.h>

#ifndef CGR_MAX_ROUTES_PI
#define CGR_MAX_ROUTES_PI 64
#endif
#ifndef CGR_MAX_ROUTES_ESP
#define CGR_MAX_ROUTES_ESP 16
#endif

#define MAX_PARETO_PER_NODE 8

void cgr_config_default(cgr_config_t *cfg, int max_routes)
{
    cfg->max_hops   = BUNDLE_MAX_HOPS;
    cfg->max_routes = max_routes > 0 ? max_routes : CGR_MAX_ROUTES_PI;
    cfg->w1         = 1.0f;
    cfg->w2         = 0.5f;
    cfg->w3         = 0.3f;
    cfg->ber_aware  = 1;
}

int cgr_contact_ber_feasible(const contact_t *c, float bundle_size_bytes)
{
    if (c->ber <= 0.0f) return 1;
    /* (1-ber)^(8*size) >= 0.5  ->  size * 8 * log10(1-ber) >= log10(0.5) is
     * not numerically nice; compute in log space. */
    float p = 1.0f - c->ber;
    if (p <= 0.0f) return 0;
    /* log(p^(8*size)) = 8*size*log(p); we want p^(8*size) >= 0.5. */
    float exponent = 8.0f * bundle_size_bytes;
    /* Compare exponent*log(p) >= log(0.5). Both negative; use float math. */
    if (exponent * logf(p) >= logf(0.5f)) return 1;
    return 0;
}

/* --- Pareto frontier signature at a node --- */
typedef struct {
    dtn_time_t t_arrive;
    float      conf;
    float      margin;
} pareto_sig_t;

typedef struct {
    pareto_sig_t sigs[MAX_PARETO_PER_NODE];
    size_t       count;
} frontier_t;

/* --- K-best beam (max-heap by cost). We keep a simple sorted array for
 * simplicity; max_routes is small (16 or 64). */
typedef struct {
    cgr_route_t routes[CGR_MAX_ROUTES_PI];  /* sized to max at compile time */
    size_t      count;
    size_t      cap;
} beam_t;

static void beam_init(beam_t *bm, size_t cap)
{
    bm->count = 0;
    bm->cap   = cap;
}

/* Insert into the beam, evicting the worst (highest-cost) route if full
 * and the new one is better. Returns 1 if inserted, 0 if rejected. */
static int beam_push(beam_t *bm, const cgr_route_t *r)
{
    if (bm->count < bm->cap) {
        bm->routes[bm->count++] = *r;
        return 1;
    }
    /* Find the worst (max-cost) entry. */
    size_t worst = 0;
    for (size_t i = 1; i < bm->count; ++i)
        if (bm->routes[i].cost > bm->routes[worst].cost) worst = i;
    if (r->cost < bm->routes[worst].cost) {
        bm->routes[worst] = *r;
        return 1;
    }
    return 0;
}

/* --- DFS stack frame. Iterative to avoid recursion on ESP32. --- */
typedef struct {
    const contact_t *contact;
    size_t           path_len;        /* number of hops so far, including this one */
    dtn_time_t       t_arrive;       /* A_i at this contact's receiver */
    node_id_t        visited[BUNDLE_MAX_HOPS + 1];
    size_t           visited_count;
    /* Iterator over the *next* contacts to expand. */
    size_t           next_idx;
} dfs_frame_t;

#define DFS_STACK_MAX (BUNDLE_MAX_HOPS + 1)

static int visited_contains(const dfs_frame_t *f, node_id_t n)
{
    for (size_t i = 0; i < f->visited_count; ++i)
        if (f->visited[i] == n) return 1;
    return 0;
}

/* Compute the cost J(alpha, rho, t). Mirrors Project_DSN's _cost. */
static float compute_cost(const cgr_config_t *cfg, const bundle_t *b,
                          const contact_t **route, size_t hop_count,
                          dtn_time_t t_now, dtn_time_t A_m)
{
    float delay = A_m - t_now;
    float conf  = 1.0f;
    for (size_t i = 0; i < hop_count; ++i) conf *= route[i]->conf;
    float log_conf = logf(conf > 1e-12f ? conf : 1e-12f);

    /* Capacity margin: min over hops of (cap_use - size), recomputing T_i forward. */
    float margin = 1e30f;
    dtn_time_t t_arr = t_now;
    for (size_t i = 0; i < hop_count; ++i) {
        const contact_t *c = route[i];
        dtn_time_t T_i = contact_earliest_start(c, t_arr);
        float m_i = contact_cap_use(c, T_i) - (float)b->size;
        if (m_i < margin) margin = m_i;
        t_arr = contact_arrival_at_receiver(c, (float)b->size, T_i);
    }
    if (!isfinite(margin)) margin = 0.0f;
    return cfg->w1 * delay - cfg->w2 * log_conf - cfg->w3 * margin;
}

int cgr_select_route(const contact_plan_t *cp, const bundle_t *b,
                     dtn_time_t t, const cgr_config_t *cfg,
                     cgr_route_t *route_out)
{
    /* Cap the beam at the compile-time array bound. */
    size_t beam_cap = (size_t)cfg->max_routes;
    if (beam_cap > CGR_MAX_ROUTES_PI) beam_cap = CGR_MAX_ROUTES_PI;

    beam_t beam;
    beam_init(&beam, beam_cap);

    /* Per-node Pareto frontier. We keep a fixed-size table indexed by a
     * small map (node_id -> frontier_t). For the MVP with 5 nodes this is
     * fine; use a simple linear scan. */
    frontier_t frontiers[8];
    node_id_t  frontier_nodes[8];
    size_t     frontier_count = 0;

    /* Seed: contacts departing from the bundle's current node. */
    const contact_t *seeds[CONTACT_PLAN_MAX];
    size_t n_seeds = contact_plan_from_node(cp, b->curr, seeds, CONTACT_PLAN_MAX);

    for (size_t s = 0; s < n_seeds; ++s) {
        const contact_t *seed = seeds[s];
        if (seed->status == CONTACT_DOWN) continue;
        if (!(contact_is_active(seed, t) || contact_is_future(seed, t))) continue;

        /* DFS stack. */
        dfs_frame_t stack[DFS_STACK_MAX];
        int top = 0;
        memset(&stack[0], 0, sizeof(stack[0]));
        stack[0].contact       = seed;
        stack[0].path_len      = 1;
        stack[0].t_arrive      = t;   /* will compute below */
        stack[0].visited[0]    = b->curr;
        stack[0].visited_count = 1;
        stack[0].next_idx      = 0;

        /* Evaluate the seed contact. */
        dtn_time_t T_i = contact_earliest_start(seed, t);
        if (T_i >= seed->t_end) continue;
        if (T_i + contact_tx_duration(seed, (float)b->size) > seed->t_end) continue;
        dtn_time_t A_i = contact_arrival_at_receiver(seed, (float)b->size, T_i);
        if (A_i >= bundle_t_exp(b)) continue;
        if (contact_cap_use(seed, T_i) < (float)b->size) continue;
        if (cfg->ber_aware && !cgr_contact_ber_feasible(seed, (float)b->size)) continue;
        if (seed->to == b->dst) {
            /* Direct route of length 1. */
            const contact_t *route[1] = { seed };
            cgr_route_t r;
            r.hop_count = 1;
            r.hops[0]   = seed;
            r.arrival_time = A_i;
            r.cost = compute_cost(cfg, b, route, 1, t, A_i);
            beam_push(&beam, &r);
            continue;
        }
        stack[0].t_arrive = A_i;
        /* Record Pareto signature at seed->to. */
        /* (deferred; we check on expansion) */

        while (top >= 0) {
            dfs_frame_t *f = &stack[top];
            if (f->path_len > (size_t)cfg->max_hops) { if (--top < 0) break; continue; }

            /* Expand next contacts from the current contact's receiver. */
            const contact_t *nxt[CONTACT_PLAN_MAX];
            size_t n_nxt = contact_plan_from_node(cp, f->contact->to, nxt, CONTACT_PLAN_MAX);

            int expanded_any = 0;
            for (; f->next_idx < n_nxt; ++f->next_idx) {
                const contact_t *nc = nxt[f->next_idx];
                if (nc->status == CONTACT_DOWN) continue;
                if (!(contact_is_active(nc, f->t_arrive) || contact_is_future(nc, f->t_arrive))) continue;

                /* Pruning 1: no node revisits. */
                if (visited_contains(f, nc->to)) continue;

                /* Timing. */
                dtn_time_t T_n = contact_earliest_start(nc, f->t_arrive);
                if (T_n >= nc->t_end) continue;
                if (T_n + contact_tx_duration(nc, (float)b->size) > nc->t_end) continue;
                dtn_time_t A_n = contact_arrival_at_receiver(nc, (float)b->size, T_n);
                if (A_n >= bundle_t_exp(b)) continue;
                if (contact_cap_use(nc, T_n) < (float)b->size) continue;
                if (cfg->ber_aware && !cgr_contact_ber_feasible(nc, (float)b->size)) continue;

                /* Pruning 2: Pareto dominance at nc->to. */
                float conf_path = 1.0f;
                for (size_t i = 0; i <= (size_t)top; ++i) conf_path *= stack[i].contact->conf;
                conf_path *= nc->conf;
                float margin_here = contact_cap_use(nc, T_n) - (float)b->size;
                pareto_sig_t sig = { A_n, conf_path, margin_here };

                /* Look up / create frontier for nc->to. */
                frontier_t *fr = NULL;
                for (size_t i = 0; i < frontier_count; ++i)
                    if (frontier_nodes[i] == nc->to) { fr = &frontiers[i]; break; }
                if (!fr && frontier_count < 8) {
                    frontier_nodes[frontier_count] = nc->to;
                    fr = &frontiers[frontier_count];
                    memset(fr, 0, sizeof(*fr));
                    ++frontier_count;
                }
                if (fr) {
                    int dominated = 0;
                    for (size_t i = 0; i < fr->count; ++i) {
                        if (fr->sigs[i].t_arrive <= sig.t_arrive &&
                            fr->sigs[i].conf    >= sig.conf &&
                            fr->sigs[i].margin  >= sig.margin) {
                            dominated = 1; break;
                        }
                    }
                    if (dominated) continue;
                    /* Insert + prune dominated. */
                    size_t w = 0;
                    for (size_t i = 0; i < fr->count; ++i) {
                        if (!(sig.t_arrive <= fr->sigs[i].t_arrive &&
                              sig.conf    >= fr->sigs[i].conf &&
                              sig.margin  >= fr->sigs[i].margin))
                            fr->sigs[w++] = fr->sigs[i];
                    }
                    fr->sigs[w++] = sig;
                    fr->count = w;
                    if (fr->count > MAX_PARETO_PER_NODE) {
                        /* Keep the smallest-t_arrive entries. */
                        for (size_t i = 0; i < fr->count; ++i)
                            for (size_t j = i + 1; j < fr->count; ++j)
                                if (fr->sigs[j].t_arrive < fr->sigs[i].t_arrive) {
                                    pareto_sig_t tmp = fr->sigs[i]; fr->sigs[i] = fr->sigs[j]; fr->sigs[j] = tmp;
                                }
                        fr->count = MAX_PARETO_PER_NODE;
                    }
                }

                /* Destination reached? */
                if (nc->to == b->dst) {
                    /* Reconstruct the route from the stack + nc. */
                    cgr_route_t r;
                    r.hop_count = 0;
                    for (int i = 0; i <= top; ++i)
                        if (r.hop_count < CGR_ROUTE_MAX_HOPS)
                            r.hops[r.hop_count++] = stack[i].contact;
                    if (r.hop_count < CGR_ROUTE_MAX_HOPS)
                        r.hops[r.hop_count++] = nc;
                    r.arrival_time = A_n;
                    r.cost = compute_cost(cfg, b, r.hops, r.hop_count, t, A_n);
                    beam_push(&beam, &r);
                    continue;   /* don't expand past destination */
                }

                /* Push the next frame. */
                if (top + 1 >= DFS_STACK_MAX) continue;
                dfs_frame_t *nf = &stack[top + 1];
                memset(nf, 0, sizeof(*nf));
                nf->contact       = nc;
                nf->path_len      = f->path_len + 1;
                nf->t_arrive      = A_n;
                nf->visited_count = f->visited_count;
                for (size_t i = 0; i < f->visited_count && i < BUNDLE_MAX_HOPS; ++i)
                    nf->visited[i] = f->visited[i];
                if (nf->visited_count < BUNDLE_MAX_HOPS + 1)
                    nf->visited[nf->visited_count++] = nc->to;
                nf->next_idx = 0;
                ++top;
                expanded_any = 1;
                break;   /* restart expansion from the new top */
            }
            if (!expanded_any && f->next_idx >= n_nxt) {
                /* Frame exhausted; pop. */
                --top;
            }
        }
    }

    if (beam.count == 0) return -1;

    /* Pick the min-cost route. */
    size_t best = 0;
    for (size_t i = 1; i < beam.count; ++i)
        if (beam.routes[i].cost < beam.routes[best].cost) best = i;
    *route_out = beam.routes[best];
    return 0;
}