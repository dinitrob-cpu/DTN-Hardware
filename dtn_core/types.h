#ifndef DTN_CORE_TYPES_H
#define DTN_CORE_TYPES_H

#include <stdint.h>

/* Node ID: 4-char string (EART, ERLY, MRLY, MLND, ROVR). Stored as a
 * 32-bit value (big-endian) for compact wire encoding. Helpers below. */
typedef uint32_t node_id_t;

#define NODE_ID_LEN 4

/* Encode/decode between the 4-char string and the 32-bit wire form. */
node_id_t node_id_from_str(const char *s);
void      node_id_to_str(node_id_t id, char out[5]); /* out is NUL-terminated */

/* Timestamp: simulation time in seconds since boot/epoch. float gives
 * ~7 decimal digits — plenty for contact windows on the order of minutes
 * and owlt on the order of seconds. */
typedef float dtn_time_t;

#endif /* DTN_CORE_TYPES_H */