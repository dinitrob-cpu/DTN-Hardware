/* dtn_core/types.c — node ID helpers. */
#include "types.h"
#include <string.h>
#include <arpa/inet.h>  /* htonl/ntohl (works on Linux; ESP-IDF provides them too) */

node_id_t node_id_from_str(const char *s)
{
    /* Pack 4 chars into a big-endian uint32. Short strings are left-padded
     * with spaces so "AB" becomes "AB  " (two trailing spaces). */
    char buf[4] = {' ', ' ', ' ', ' '};
    for (int i = 0; i < 4 && s[i] != '\0'; ++i)
        buf[i] = s[i];
    uint32_t v = ((uint32_t)(uint8_t)buf[0] << 24) |
                 ((uint32_t)(uint8_t)buf[1] << 16) |
                 ((uint32_t)(uint8_t)buf[2] <<  8) |
                 ((uint32_t)(uint8_t)buf[3]);
    return v;  /* stored host-order; wire order applied at serialize time */
}

void node_id_to_str(node_id_t id, char out[5])
{
    out[0] = (char)((id >> 24) & 0xFF);
    out[1] = (char)((id >> 16) & 0xFF);
    out[2] = (char)((id >>  8) & 0xFF);
    out[3] = (char)( id        & 0xFF);
    out[4] = '\0';
}