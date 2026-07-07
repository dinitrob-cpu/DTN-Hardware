/* dtn_core/contact.c — contact + contact plan logic.
 * Port of Project_DSN's core/contact.py. JSON parsing uses a tiny
 * hand-rolled reader (no external dep) suitable for the fixed contact_plan.json. */
#include "contact.h"
#include "types.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

int contact_is_active(const contact_t *c, dtn_time_t t)
{
    return (c->t_start <= t && t < c->t_end) && (c->status == CONTACT_UP);
}

int contact_is_future(const contact_t *c, dtn_time_t t)
{
    return (t < c->t_start) && (c->status != CONTACT_DOWN);
}

float contact_duration(const contact_t *c)
{
    float d = c->t_end - c->t_start;
    return d > 0.0f ? d : 0.0f;
}

float contact_capacity(const contact_t *c)
{
    return c->rate * contact_duration(c);
}

float contact_cap_rem(const contact_t *c, dtn_time_t t)
{
    dtn_time_t time_left = c->t_end - t;
    if (time_left < 0.0f) time_left = 0.0f;
    return c->rate * time_left - c->bytes_used;
}

float contact_cap_use(const contact_t *c, dtn_time_t t)
{
    return contact_cap_rem(c, t) - c->reserved;
}

void contact_reserve(contact_t *c, float size)             { c->reserved += size; }
void contact_release_reservation(contact_t *c, float size){ if (c->reserved > size) c->reserved -= size; else c->reserved = 0.0f; }
void contact_consume(contact_t *c, float size)
{
    c->bytes_used += size;
    if (c->reserved > size) c->reserved -= size; else c->reserved = 0.0f;
}

int contact_is_feasible_for(const contact_t *c, float bundle_size, dtn_time_t t)
{
    return contact_is_active(c, t) && (contact_cap_use(c, t) >= bundle_size);
}

dtn_time_t contact_tx_duration(const contact_t *c, float bundle_size)
{
    return c->rate > 0.0f ? (dtn_time_t)(bundle_size / c->rate) : (dtn_time_t)1e30f;
}

dtn_time_t contact_earliest_start(const contact_t *c, dtn_time_t arrival_time)
{
    return arrival_time > c->t_start ? arrival_time : c->t_start;
}

dtn_time_t contact_arrival_at_receiver(const contact_t *c, float bundle_size, dtn_time_t start_time)
{
    return start_time + contact_tx_duration(c, bundle_size) + c->owlt;
}

/* --- Contact plan --- */

void contact_plan_add(contact_plan_t *cp, const contact_t *c)
{
    if (cp->count < CONTACT_PLAN_MAX) {
        cp->contacts[cp->count++] = *c;
    }
}

const contact_t *contact_plan_get(const contact_plan_t *cp, node_id_t frm, node_id_t to)
{
    for (size_t i = 0; i < cp->count; ++i) {
        if (cp->contacts[i].frm == frm && cp->contacts[i].to == to)
            return &cp->contacts[i];
    }
    return NULL;
}

size_t contact_plan_from_node(const contact_plan_t *cp, node_id_t frm,
                             const contact_t **out, size_t out_cap)
{
    size_t n = 0;
    for (size_t i = 0; i < cp->count; ++i) {
        if (cp->contacts[i].frm == frm) {
            if (n < out_cap) out[n] = &cp->contacts[i];
            ++n;
        }
    }
    return n;
}

/* --- Tiny JSON reader for contact_plan.json ---
 * We don't link a JSON library. The file has a known, flat shape:
 *   { "contacts": [ {"frm":"EART","to":"ERLY","t_start":0,"t_end":60,
 *                    "rate":1200,"owlt":0,"conf":1.0,"ber":0.0}, ... ] }
 * The parser below tokenizes just enough to extract those fields.
 * It is NOT a general JSON parser; it validates the keys it expects. */

static const char *find_key(const char *p, const char *key)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *hit = strstr(p, pat);
    return hit;
}

static const char *skip_ws(const char *p)
{
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    return p;
}

static const char *read_string_field(const char *p, char *out, size_t cap)
{
    /* p points just after "key": — skip ws, expect '"'. */
    p = skip_ws(p);
    if (*p != '"') return NULL;
    ++p;
    size_t i = 0;
    while (*p && *p != '"' && i < cap - 1) out[i++] = *p++;
    out[i] = '\0';
    if (*p == '"') ++p;
    return p;
}

static const char *read_number(const char *p, float *out)
{
    p = skip_ws(p);
    char *end;
    float v = strtof(p, &end);
    if (end == p) return NULL;
    *out = v;
    return end;
}

int contact_plan_load_json(contact_plan_t *cp, const char *json_path)
{
    FILE *f = fopen(json_path, "rb");
    if (!f) return -1;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    cp->count = 0;
    const char *p = buf;
    while ((p = find_key(p, "frm")) != NULL) {
        /* p points at the start of "frm". Find the colon after it. */
        const char *colon = strchr(p, ':');
        if (!colon) break;
        char frm_str[8], to_str[8];
        float t_start = 0, t_end = 0, rate = 0, owlt = 0, conf = 1.0f, ber = 0.0f;

        const char *q = read_string_field(colon + 1, frm_str, sizeof(frm_str));
        if (!q) { p += 4; continue; }

        const char *k_to = find_key(q, "to");
        if (!k_to) break;
        colon = strchr(k_to, ':');
        q = read_string_field(colon + 1, to_str, sizeof(to_str));
        if (!q) { p = k_to + 3; continue; }

        const char *k;
        if ((k = find_key(q, "t_start")) && (colon = strchr(k, ':')))
            q = read_number(colon + 1, &t_start);
        if ((k = find_key(q, "t_end"))   && (colon = strchr(k, ':')))
            q = read_number(colon + 1, &t_end);
        if ((k = find_key(q, "rate"))    && (colon = strchr(k, ':')))
            q = read_number(colon + 1, &rate);
        if ((k = find_key(q, "owlt"))    && (colon = strchr(k, ':')))
            q = read_number(colon + 1, &owlt);
        if ((k = find_key(q, "conf"))    && (colon = strchr(k, ':')))
            q = read_number(colon + 1, &conf);
        if ((k = find_key(q, "ber"))     && (colon = strchr(k, ':')))
            q = read_number(colon + 1, &ber);

        contact_t c;
        memset(&c, 0, sizeof(c));
        c.frm     = node_id_from_str(frm_str);
        c.to      = node_id_from_str(to_str);
        c.t_start = t_start;
        c.t_end   = t_end;
        c.rate    = rate;
        c.owlt    = owlt;
        c.conf    = conf;
        c.ber     = ber;
        c.status  = CONTACT_UP;
        contact_plan_add(cp, &c);

        /* Advance past this object to find the next "frm". */
        p = q;
    }
    return (int)cp->count;
}