#ifndef TRANSPORT_LORA_TRANSPORT_H
#define TRANSPORT_LORA_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

/* LoRa transport interface. Two implementations:
 *   lora_pi.c     — Linux userspace, SPI via /dev/spidev0.0, IRQ via GPIO sysfs.
 *   lora_esp32.c  — ESP-IDF, SPI driver + DIO0 GPIO interrupt + FreeRTOS task.
 * Both expose the same surface so node/ code is identical across boards. */

typedef struct {
    float    freq_mhz;     /* 868.0 for EU, 915.0 for US */
    uint8_t  sf;           /* spreading factor 7..12 (12 = long range, slow) */
    uint32_t bw_hz;        /* bandwidth in Hz (125000 is typical) */
    uint8_t  tx_power_dbm; /* 14 for EU 868 legal EIRP */
    uint32_t coding_rate;  /* 4/5 = 5, 4/6 = 6, 4/7 = 7, 4/8 = 8 */
    uint32_t preamble_len; /* typically 8 */
    int      sync_word;    /* 0x34 (private) or 0x12 (LoRaWAN) */
    /* Platform-specific pin/GPIO settings — interpreted by the impl. */
    int      cs_pin;       /* Pi: GPIO8 (CE0). ESP32: GPIO10. */
    int      reset_pin;    /* Pi: GPIO22. ESP32: GPIO5. */
    int      dio0_pin;     /* Pi: GPIO25. ESP32: GPIO6. */
    int      dio1_pin;     /* optional; -1 if unused */
} lora_config_t;

typedef struct lora_transport lora_transport_t;

struct lora_transport_vtable {
    int  (*init)(lora_transport_t *t, const lora_config_t *cfg);
    int  (*send)(lora_transport_t *t, const uint8_t *buf, size_t len);
    /* Blocking receive with timeout. Returns bytes received, or:
     *   0  -> timeout
     *  -1  -> error */
    int  (*recv)(lora_transport_t *t, uint8_t *buf, size_t cap, uint32_t timeout_ms);
    int  (*rssi)(lora_transport_t *t);   /* dBm, negative */
    void (*close)(lora_transport_t *t);
    int  (*get_irq_fd)(lora_transport_t *t);  /* returns poll()-able fd, or -1 */
};

struct lora_transport {
    const struct lora_transport_vtable *vtable;
    void *impl;   /* platform-specific state */
};

/* Convenience wrappers. */
int  lora_init (lora_transport_t *t, const lora_config_t *cfg);
int  lora_send (lora_transport_t *t, const uint8_t *buf, size_t len);
int  lora_recv (lora_transport_t *t, uint8_t *buf, size_t cap, uint32_t timeout_ms);
int  lora_rssi (lora_transport_t *t);
void lora_close(lora_transport_t *t);

/* Returns a file descriptor that becomes readable when the radio raises
 * an interrupt (DIO0 edge: TX_DONE or RX_DONE). Returns -1 if the transport
 * doesn't support edge events (caller should fall back to polling).
 * The caller can poll()/epoll() on this fd. */
int  lora_get_irq_fd(lora_transport_t *t);

/* Build a default 868 MHz config for the given platform pins. */
void lora_config_default_868(lora_config_t *cfg, int cs, int reset, int dio0, int dio1);

/* --- Platform-specific constructors ---
 * Each implementation exposes an init function that wires its vtable +
 * impl state into a lora_transport_t. The node app allocates the state
 * struct (defined here so the caller has the size) and passes it in. */

#ifdef __linux__
/* lora_pi.c */
struct pi_lora_state {
    int     spi_fd;
    int     gpiochip_fd;
    int     dio0_line;
    int     dio0_fd;          /* GPIO V2 line event fd (poll()-able) */
    uint8_t op_mode;
};
typedef struct pi_lora_state pi_lora_state_t;
void lora_transport_init_pi(lora_transport_t *t, pi_lora_state_t *st);
#endif

#ifdef ESP_PLATFORM
/* lora_esp32.c — ESP-IDF types are available under this guard. */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
struct esp_lora_state {
    spi_device_handle_t spi;
    spi_host_device_t   host;
    int                 cs_pin;
    int                 reset_pin;
    int                 dio0_pin;
    SemaphoreHandle_t   dio0_sem;
    volatile uint8_t    last_irq;
};
typedef struct esp_lora_state esp_lora_state_t;
void lora_transport_init_esp32(lora_transport_t *t, esp_lora_state_t *st);
#endif

#endif /* TRANSPORT_LORA_TRANSPORT_H */