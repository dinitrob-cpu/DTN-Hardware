/* dtn_core/bundle.c — bundle logic + wire format.
 *
 * Wire format (single LoRa frame, little-endian):
 *   offset  field
 *   0       magic        u16  = 0x4454 ("DT")
 *   2       version     u8   = 1
 *   3       bundle_id   u64
 *   11      src         u32  (node_id, host-order packed big-endian)
 *   15      dst         u32
 *   19      curr        u32
 *   23      cust        u32
 *   27      size        u16  (total on-wire size incl. crc, big-endian)
 *   29      prio        u8
 *   30      class        u8
 *   31      t_gen        f32
 *   35      ttl          f32
 *   39      payload_len  u16
 *   41      payload[N]
 *   41+N    crc32        u32  (over bytes 0 .. 41+N-1)
 *
 * Header is 41 bytes; with up to 160 bytes payload + 4 crc = 205 bytes max,
 * well under the 222-byte RFM95 limit. */
#include "bundle.h"
#include <string.h>
#include <math.h>

/* CRC32 (IEEE 802.3 polynomial, reflected). Table-free for portability. */
static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= buf[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return ~crc;
}

/* Big-endian byte writers (so frames are byte-identical on Pi and ESP32). */
static void put_u16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static void put_u32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}
static void put_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8*(7-i)));
}
static void put_f32(uint8_t *p, float f) {
    uint32_t v; memcpy(&v, &f, 4);
    put_u32(p, v);
}
static uint16_t get_u16(const uint8_t *p) { return ((uint16_t)p[0]<<8) | p[1]; }
static uint32_t get_u32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) |
           ((uint32_t)p[2]<<8)  |  (uint32_t)p[3];
}
static uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}
static float get_f32(const uint8_t *p) {
    uint32_t v = get_u32(p); float f; memcpy(&f, &v, 4); return f;
}

#define WIRE_MAGIC   0x4454u
#define WIRE_VERSION 1u
#define WIRE_HDR_LEN 41u

dtn_time_t bundle_age(const bundle_t *b, dtn_time_t t)   { return t - b->t_gen; }
dtn_time_t bundle_rttl(const bundle_t *b, dtn_time_t t)  { return b->ttl - bundle_age(b, t); }
dtn_time_t bundle_t_exp(const bundle_t *b)              { return b->t_gen + b->ttl; }
int        bundle_is_alive(const bundle_t *b, dtn_time_t t) { return bundle_rttl(b, t) > 0.0f; }

float bundle_priority_score(const bundle_t *b)
{
    int span = BUNDLE_PRIO_MAX - BUNDLE_PRIO_MIN;
    if (span == 0) return 1.0f;
    return (float)(b->prio - BUNDLE_PRIO_MIN) / (float)span;
}

float bundle_urgency(const bundle_t *b, dtn_time_t t, float eps)
{
    float rttl = bundle_rttl(b, t);
    if (eps < rttl && eps > 0.0f) return 1.0f / rttl;
    return 1.0f / eps;
}

float bundle_scheduling_key(const bundle_t *b, dtn_time_t t,
                            float lam1, float lam2, float lam3, float lam4,
                            float size_penalty_threshold)
{
    float cust_bonus   = (b->cust != 0 && b->cust == b->curr) ? 1.0f : 0.0f;
    float size_penalty = (b->size > size_penalty_threshold)   ? 1.0f : 0.0f;
    return   lam1 * bundle_priority_score(b)
           + lam2 * bundle_urgency(b, t, 1.0f)
           + lam3 * cust_bonus
           - lam4 * size_penalty;
}

int bundle_serialize(const bundle_t *b, uint8_t *out, size_t cap)
{
    if (b->payload_len > BUNDLE_MAX_PAYLOAD) return -1;
    size_t total = WIRE_HDR_LEN + b->payload_len + 4u; /* hdr + payload + crc */
    if (total > cap) return -1;

    uint8_t *p = out;
    put_u16(p, WIRE_MAGIC); p += 2;
    *p++ = (uint8_t)WIRE_VERSION;
    put_u64(p, b->bundle_id); p += 8;
    put_u32(p, b->src);   p += 4;
    put_u32(p, b->dst);   p += 4;
    put_u32(p, b->curr);  p += 4;
    put_u32(p, b->cust);  p += 4;
    put_u16(p, (uint16_t)total); p += 2;
    *p++ = b->prio;
    *p++ = (uint8_t)b->bundle_class;
    put_f32(p, b->t_gen); p += 4;
    put_f32(p, b->ttl);   p += 4;
    put_u16(p, b->payload_len); p += 2;
    if (b->payload_len) memcpy(p, b->payload, b->payload_len);
    p += b->payload_len;

    uint32_t crc = crc32_update(0, out, (size_t)(p - out));
    put_u32(p, crc); p += 4;
    return (int)(p - out);
}

int bundle_deserialize(bundle_t *b, const uint8_t *in, size_t len)
{
    if (len < WIRE_HDR_LEN + 4u) return -1;
    if (get_u16(in) != WIRE_MAGIC) return -1;
    if (in[2] != WIRE_VERSION)     return -1;

    uint16_t total = get_u16(in + 27);
    if (total != len) return -1;   /* size mismatch -> corrupt */

    const uint8_t *p = in + 3;
    b->bundle_id = get_u64(p); p += 8;
    b->src  = get_u32(p); p += 4;
    b->dst  = get_u32(p); p += 4;
    b->curr = get_u32(p); p += 4;
    b->cust = get_u32(p); p += 4;
    b->size = get_u16(p); p += 2;
    b->prio = *p++;
    b->bundle_class = (bundle_class_t)(*p++);
    b->t_gen = get_f32(p); p += 4;
    b->ttl   = get_f32(p); p += 4;
    b->payload_len = get_u16(p); p += 2;

    if (b->payload_len > BUNDLE_MAX_PAYLOAD) return -1;
    if ((size_t)(p - in) + b->payload_len + 4u != len) return -1;
    if (b->payload_len) memcpy(b->payload, p, b->payload_len);
    p += b->payload_len;

    uint32_t crc_calc = crc32_update(0, in, (size_t)(p - in));
    uint32_t crc_in   = get_u32(p);
    if (crc_calc != crc_in) return -1;

    /* Status fields are not on the wire; caller initializes them. */
    b->status     = BUNDLE_QUEUED;
    b->retries    = 0;
    b->next_hop   = 0;
    b->t_sched    = 0;
    b->t_tx_start = 0;
    b->t_arr      = 0;
    return 0;
}