#ifndef DTN_CORE_BUNDLE_H
#define DTN_CORE_BUNDLE_H

#include "types.h"
#include <stddef.h>

/* Bundle status (mirrors Project_DSN's BundleStatus). */
typedef enum {
    BUNDLE_GENERATED = 0,
    BUNDLE_QUEUED,
    BUNDLE_ROUTING,
    BUNDLE_WAITING,
    BUNDLE_TRANSMITTING,
    BUNDLE_IN_TRANSIT,
    BUNDLE_CUSTODY,
    BUNDLE_DELIVERED,
    BUNDLE_DISRUPTED,
    BUNDLE_EXPIRED,
    BUNDLE_TERMINATED
} bundle_status_t;

/* Bundle class (mirrors Project_DSN's BundleClass). Priority ordering:
 * COMMAND > SCIENCE > TELEMETRY > HOUSEKEEPING. */
typedef enum {
    BUNDLE_CLASS_HOUSEKEEPING = 0,
    BUNDLE_CLASS_TELEMETRY     = 1,
    BUNDLE_CLASS_SCIENCE       = 2,
    BUNDLE_CLASS_COMMAND       = 3
} bundle_class_t;

#define BUNDLE_PRIO_MIN 0
#define BUNDLE_PRIO_MAX 3

/* Max payload in a single (unfragmented) bundle. A LoRa frame on RFM95
 * can carry up to ~222 bytes; we reserve ~60 bytes for the header + crc,
 * leaving 160 bytes of payload. M6 will add fragmentation for larger
 * payloads; for now payloads over BUNDLE_MAX_PAYLOAD are rejected. */
#define BUNDLE_MAX_PAYLOAD 160

/* Max hops in a route (matches Project_DSN's default). */
#define BUNDLE_MAX_HOPS 10

typedef struct {
    /* --- Core token fields (Phase 1) --- */
    uint64_t        bundle_id;   /* 64-bit unique id */
    node_id_t       src;
    node_id_t       dst;
    node_id_t       curr;        /* current custodian/location */
    uint16_t        size;        /* total size in bytes (header+payload on wire) */
    uint8_t         prio;        /* 0..3, higher = more important */
    bundle_class_t  bundle_class;
    dtn_time_t      t_gen;       /* generation time (sim seconds) */
    dtn_time_t      ttl;         /* time-to-live (seconds) */

    /* --- Mutable protocol fields --- */
    node_id_t       cust;        /* current custodian (0 if none) */
    node_id_t       next_hop;     /* selected next hop (0 if none) */
    bundle_status_t status;
    uint8_t         retries;

    /* --- Routing-control attributes --- */
    dtn_time_t      t_sched;     /* time forwarding was scheduled */
    dtn_time_t      t_tx_start;  /* actual transmission start */
    dtn_time_t      t_arr;       /* predicted/actual arrival at next hop */

    /* --- Payload --- */
    uint16_t        payload_len;
    uint8_t         payload[BUNDLE_MAX_PAYLOAD];
} bundle_t;

/* Helpers (port of Project_DSN's bundle.py time helpers). */
dtn_time_t bundle_age   (const bundle_t *b, dtn_time_t t);
dtn_time_t bundle_rttl  (const bundle_t *b, dtn_time_t t);
dtn_time_t bundle_t_exp (const bundle_t *b);
int        bundle_is_alive(const bundle_t *b, dtn_time_t t);

/* Priority score P(alpha) in [0,1]. */
float bundle_priority_score(const bundle_t *b);

/* Urgency Urg(alpha, t) = 1 / max(eps, RTTL). */
float bundle_urgency(const bundle_t *b, dtn_time_t t, float eps);

/* Composite scheduling key (higher = served first). */
float bundle_scheduling_key(const bundle_t *b, dtn_time_t t,
                            float lam1, float lam2, float lam3, float lam4,
                            float size_penalty_threshold);

/* Wire format serialization. Returns bytes written, or -1 on overflow.
 * `cap` is the capacity of `out`. The frame is self-contained: header +
 * payload + crc32. */
int  bundle_serialize(const bundle_t *b, uint8_t *out, size_t cap);

/* Deserialization. Returns 0 on success, -1 on bad magic/version/crc/size.
 * On success `b` is populated. */
int  bundle_deserialize(bundle_t *b, const uint8_t *in, size_t len);

#endif /* DTN_CORE_BUNDLE_H */