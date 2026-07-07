/* transport/lora_common.c — shared vtable wrappers + config helper.
 *
 * The actual send/recv/init are in lora_pi.c (Linux) or lora_esp32.c (ESP-IDF).
 * This file compiles on both platforms and just dispatches through the vtable.
 */
#include "lora_transport.h"

int lora_init(lora_transport_t *t, const lora_config_t *cfg)
{
    return t->vtable->init ? t->vtable->init(t, cfg) : -1;
}

int lora_send(lora_transport_t *t, const uint8_t *buf, size_t len)
{
    return t->vtable->send ? t->vtable->send(t, buf, len) : -1;
}

int lora_recv(lora_transport_t *t, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    return t->vtable->recv ? t->vtable->recv(t, buf, cap, timeout_ms) : -1;
}

int lora_rssi(lora_transport_t *t)
{
    return t->vtable->rssi ? t->vtable->rssi(t) : 0;
}

void lora_close(lora_transport_t *t)
{
    if (t->vtable->close) t->vtable->close(t);
}

void lora_config_default_868(lora_config_t *cfg, int cs, int reset, int dio0, int dio1)
{
    cfg->freq_mhz      = 868.0f;
    cfg->sf            = 12;          /* long-range, slow — deep-space feel */
    cfg->bw_hz         = 125000;
    cfg->tx_power_dbm  = 14;          /* EU 868 EIRP ceiling */
    cfg->coding_rate   = 5;           /* 4/5 */
    cfg->preamble_len  = 8;
    cfg->sync_word     = 0x34;        /* private network sync word */
    cfg->cs_pin        = cs;
    cfg->reset_pin     = reset;
    cfg->dio0_pin      = dio0;
    cfg->dio1_pin      = dio1;
}