/* transport/lora_pi.c — Linux userspace LoRa driver for RFM95/SX1276 on a Pi 4.
 *
 * Uses /dev/spidev0.0 for SPI and the GPIO character device (or sysfs) for
 * the DIO0 IRQ. No external dependency: we drive the SX1276 registers
 * directly over spidev.
 *
 * This is a minimal but functional driver: init, send (blocking until TX_DONE),
 * recv (poll DIO0 with a timeout), and RSSI read. CAD mode and DIO1 are
 * not used in the MVP — send/recv are half-duplex and the node app serializes
 * access to the radio.
 *
 * NOTE: needs root (or the right udev rules) to open /dev/spidev0.0 and
 * /dev/gpiochip0. Run the node app with sudo.
 */
#include "lora_transport.h"

#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <linux/gpio.h>

/* --- SX1276 register map (subset) --- */
#define REG_FIFO                 0x00
#define REG_OP_MODE              0x01
#define REG_FRF_MSB              0x06
#define REG_FRF_MID              0x07
#define REG_FRF_LSB              0x08
#define REG_PA_CONFIG            0x09
#define REG_FIFO_ADDR_PTR        0x0D
#define REG_FIFO_TX_BASE         0x0E
#define REG_FIFO_RX_BASE         0x0F
#define REG_FIFO_RX_CURR         0x10
#define REG_IRQ_MASK             0x11
#define REG_IRQ_FLAGS            0x12
#define REG_RX_NB_BYTES          0x13
#define REG_PKT_SNR_VALUE        0x19
#define REG_PKT_RSSI_VALUE       0x1A
#define REG_MODEM_CONFIG_1       0x1D
#define REG_MODEM_CONFIG_2       0x1E
#define REG_MODEM_CONFIG_3       0x26
#define REG_PREAMBLE_MSB         0x20
#define REG_PREAMBLE_LSB         0x21
#define REG_PAYLOAD_LENGTH        0x22
#define REG_SYNC_WORD            0x39

/* Modes */
#define MODE_SLEEP              0x00
#define MODE_STDBY              0x01
#define MODE_TX                 0x03
#define MODE_RX_CONT            0x05
#define MODE_RX_SINGLE          0x06
#define MODE_LORA                0x80

/* IRQ flags */
#define IRQ_TX_DONE_MASK        0x08
#define IRQ_RX_DONE_MASK        0x40
#define IRQ_PAYLOAD_CRC_ERR     0x20

#define SPI_SPEED_HZ 500000

/* pi_lora_state struct is defined in lora_transport.h (under #ifdef __linux__). */

static int spi_write_read(int fd, uint8_t *buf, size_t len)
{
    struct spi_ioc_transfer tr;
    memset(&tr, 0, sizeof(tr));
    tr.tx_buf        = (unsigned long)buf;
    tr.rx_buf        = (unsigned long)buf;
    tr.len           = (uint32_t)len;
    tr.speed_hz      = SPI_SPEED_HZ;
    tr.bits_per_word = 8;
    return ioctl(fd, SPI_IOC_MESSAGE(1), &tr) < 0 ? -1 : 0;
}

static uint8_t reg_read(int fd, uint8_t addr)
{
    uint8_t buf[2] = { (uint8_t)(addr & 0x7F), 0x00 };
    spi_write_read(fd, buf, 2);
    return buf[1];
}

static void reg_write(int fd, uint8_t addr, uint8_t val)
{
    uint8_t buf[2] = { (uint8_t)(addr | 0x80), val };
    spi_write_read(fd, buf, 2);
}

static void set_mode(int fd, uint8_t mode, pi_lora_state_t *st)
{
    reg_write(fd, REG_OP_MODE, (uint8_t)(MODE_LORA | mode));
    st->op_mode = (uint8_t)(MODE_LORA | mode);
}

/* --- vtable methods --- */

static int pi_init(lora_transport_t *t, const lora_config_t *cfg)
{
    pi_lora_state_t *st = (pi_lora_state_t *)t->impl;
    if (!st) return -1;

    /* Open SPI. */
    st->spi_fd = open("/dev/spidev0.0", O_RDWR);
    if (st->spi_fd < 0) { perror("open spidev0.0"); return -1; }

    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    ioctl(st->spi_fd, SPI_IOC_WR_MODE, &mode);
    ioctl(st->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);

    /* Toggle RESET (brief pulse via GPIO). For simplicity, we rely on the
     * board already being wired; the Pi app toggles reset before init. */

    /* Set mode SLEEP to allow frequency/mode config. */
    set_mode(st->spi_fd, MODE_SLEEP, st);

    /* Frequency: FRF = freq * 2^19 / 32 MHz. */
    float frf = cfg->freq_mhz * 4.0f * 4.0f * 4.0f * 4.0f * 4.0f * 4.0f * 4.0f * 4.0f * 4.0f * 4.0f * 4.0f * 4.0f * 4.0f * 4.0f * 4.0f * 4.0f / 33554432.0f;
    uint32_t frf_int = (uint32_t)(cfg->freq_mhz * (1u << 19) / 32.0f);
    reg_write(st->spi_fd, REG_FRF_MSB, (uint8_t)(frf_int >> 16));
    reg_write(st->spi_fd, REG_FRF_MID, (uint8_t)(frf_int >> 8));
    reg_write(st->spi_fd, REG_FRF_LSB, (uint8_t)frf_int);
    (void)frf;

    /* PA config: boost + max power. */
    reg_write(st->spi_fd, REG_PA_CONFIG, (uint8_t)(0x80 | (cfg->tx_power_dbm & 0x0F)));

    /* LNA: max gain. */
    reg_write(st->spi_fd, 0x0C, 0x23);

    /* Modem config 1: BW | CR | implicit header. BW=125kHz => 0x07<<4, CR=4/5 => 0x02<<1, explicit header (0). */
    uint8_t bw_code = 0x07; /* 125 kHz */
    uint8_t cr_code = (uint8_t)((cfg->coding_rate - 4) & 0x07);
    reg_write(st->spi_fd, REG_MODEM_CONFIG_1, (uint8_t)((bw_code << 4) | (cr_code << 1) | 0x00));

    /* Modem config 2: SF | CRC on | TX mode. */
    reg_write(st->spi_fd, REG_MODEM_CONFIG_2, (uint8_t)((cfg->sf << 4) | 0x04));
    /* Symbol timeout (MSB only) — 0 means no timeout in RX_SINGLE. */
    reg_write(st->spi_fd, 0x1F, 0x00);

    /* Preamble length. */
    reg_write(st->spi_fd, REG_PREAMBLE_MSB, (uint8_t)((cfg->preamble_len >> 8) & 0xFF));
    reg_write(st->spi_fd, REG_PREAMBLE_LSB, (uint8_t)(cfg->preamble_len & 0xFF));

    /* Sync word. */
    reg_write(st->spi_fd, REG_SYNC_WORD, (uint8_t)cfg->sync_word);

    /* FIFO base addresses. */
    reg_write(st->spi_fd, REG_FIFO_TX_BASE, 0x00);
    reg_write(st->spi_fd, REG_FIFO_RX_BASE, 0x00);

    /* Open GPIO for DIO0 IRQ via /dev/gpiochip0.
     * Use the GPIO line-event interface so we get a poll()-able fd that
     * becomes readable on DIO0 rising edge (TX_DONE or RX_DONE).
     * We try the V1 (struct gpioevent_request) interface first — it's
     * available on kernels >= 5.10 (including CI runners). The V2 API
     * (gpio_v2_line_event_request) is newer and not universally available. */
    st->gpiochip_fd = open("/dev/gpiochip0", O_RDWR);
    if (st->gpiochip_fd < 0) {
        /* Non-fatal: send/recv will fall back to polling the IRQ_FLAGS
         * register (slower, higher CPU, but functional). */
        st->gpiochip_fd = -1;
        st->dio0_fd     = -1;
    } else {
        struct gpioevent_request req;
        memset(&req, 0, sizeof(req));
        req.lineoffset  = (uint32_t)cfg->dio0_pin;
        req.handleflags = GPIOHANDLE_REQUEST_INPUT;
        req.eventflags  = GPIOEVENT_REQUEST_RISING_EDGE;
        snprintf(req.consumer_label, sizeof(req.consumer_label), "dtn-lora-dio0");
        if (ioctl(st->gpiochip_fd, GPIO_GET_LINEEVENT_IOCTL, &req) < 0) {
            /* Fallback: no edge events. Send/recv will poll the IRQ register. */
            st->dio0_fd = -1;
        } else {
            st->dio0_fd = req.fd;
            /* Make the event fd non-blocking so poll() works cleanly. */
            int eflags = fcntl(st->dio0_fd, F_GETFL, 0);
            fcntl(st->dio0_fd, F_SETFL, eflags | O_NONBLOCK);
        }
    }

    /* Go to STDBY. */
    set_mode(st->spi_fd, MODE_STDBY, st);
    return 0;
}

static int pi_send(lora_transport_t *t, const uint8_t *buf, size_t len)
{
    pi_lora_state_t *st = (pi_lora_state_t *)t->impl;
    if (len > 222) return -1;

    set_mode(st->spi_fd, MODE_STDBY, st);
    reg_write(st->spi_fd, REG_FIFO_ADDR_PTR, 0x00);
    for (size_t i = 0; i < len; ++i)
        reg_write(st->spi_fd, REG_FIFO, buf[i]);
    reg_write(st->spi_fd, REG_PAYLOAD_LENGTH, (uint8_t)len);
    reg_write(st->spi_fd, REG_IRQ_MASK, 0x00);
    set_mode(st->spi_fd, MODE_TX, st);

    /* Wait for TX_DONE via GPIO edge event on DIO0, with fallback to
     * register polling if edge events aren't available. */
    uint8_t irq = 0;
    int got_irq = 0;

    if (st->dio0_fd >= 0) {
        struct pollfd pfd = { .fd = st->dio0_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 2000);   /* 2s timeout */
        if (pr > 0 && (pfd.revents & POLLIN)) {
            /* Drain the event. */
            struct gpioevent_data ev;
            ssize_t rd = read(st->dio0_fd, &ev, sizeof(ev));
            (void)rd;
            got_irq = 1;
        }
    } else {
        /* Fallback: poll the IRQ register. */
        for (int i = 0; i < 2000 && !(irq & IRQ_TX_DONE_MASK); ++i) {
            irq = reg_read(st->spi_fd, REG_IRQ_FLAGS);
            usleep(1000);
        }
        got_irq = (irq & IRQ_TX_DONE_MASK) != 0;
    }

    if (got_irq && st->dio0_fd >= 0)
        irq = reg_read(st->spi_fd, REG_IRQ_FLAGS);

    reg_write(st->spi_fd, REG_IRQ_FLAGS, 0xFF);   /* clear */
    set_mode(st->spi_fd, MODE_STDBY, st);
    return (irq & IRQ_TX_DONE_MASK) ? (int)len : -1;
}

static int pi_recv(lora_transport_t *t, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    pi_lora_state_t *st = (pi_lora_state_t *)t->impl;
    set_mode(st->spi_fd, MODE_STDBY, st);
    reg_write(st->spi_fd, REG_IRQ_MASK, 0x00);
    set_mode(st->spi_fd, MODE_RX_SINGLE, st);

    /* Wait for RX_DONE via GPIO edge event on DIO0, with fallback. */
    uint8_t irq = 0;
    int got_irq = 0;

    if (st->dio0_fd >= 0) {
        struct pollfd pfd = { .fd = st->dio0_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, (int)timeout_ms);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            struct gpioevent_data ev;
            ssize_t rd = read(st->dio0_fd, &ev, sizeof(ev));
            (void)rd;
            got_irq = 1;
        }
        /* pr == 0 -> timeout; pr < 0 -> error (treat as timeout). */
    } else {
        uint32_t waited = 0;
        while (!(irq & IRQ_RX_DONE_MASK) && waited < timeout_ms) {
            irq = reg_read(st->spi_fd, REG_IRQ_FLAGS);
            usleep(1000);
            ++waited;
        }
        got_irq = (irq & IRQ_RX_DONE_MASK) != 0;
    }

    if (!got_irq) {
        set_mode(st->spi_fd, MODE_STDBY, st);
        return 0;   /* timeout */
    }

    if (st->dio0_fd >= 0)
        irq = reg_read(st->spi_fd, REG_IRQ_FLAGS);

    if (irq & IRQ_PAYLOAD_CRC_ERR) {
        reg_write(st->spi_fd, REG_IRQ_FLAGS, 0xFF);
        set_mode(st->spi_fd, MODE_STDBY, st);
        return -1;
    }
    if (!(irq & IRQ_RX_DONE_MASK)) {
        set_mode(st->spi_fd, MODE_STDBY, st);
        return 0;
    }
    reg_write(st->spi_fd, REG_IRQ_FLAGS, 0xFF);

    uint8_t rx_len = reg_read(st->spi_fd, REG_RX_NB_BYTES);
    if (rx_len > cap) rx_len = (uint8_t)cap;
    uint8_t fifo_addr = reg_read(st->spi_fd, REG_FIFO_RX_CURR);
    reg_write(st->spi_fd, REG_FIFO_ADDR_PTR, fifo_addr);
    for (uint8_t i = 0; i < rx_len; ++i)
        buf[i] = reg_read(st->spi_fd, REG_FIFO);

    set_mode(st->spi_fd, MODE_STDBY, st);
    return rx_len;
}

static int pi_rssi(lora_transport_t *t)
{
    pi_lora_state_t *st = (pi_lora_state_t *)t->impl;
    int r = reg_read(st->spi_fd, REG_PKT_RSSI_VALUE);
    return r - 157;   /* RSSI = value - 157 for HF port (>860 MHz) */
}

static int pi_get_irq_fd(lora_transport_t *t)
{
    pi_lora_state_t *st = (pi_lora_state_t *)t->impl;
    return st->dio0_fd;
}

static void pi_close(lora_transport_t *t)
{
    pi_lora_state_t *st = (pi_lora_state_t *)t->impl;
    if (st->dio0_fd >= 0)     close(st->dio0_fd);
    if (st->spi_fd >= 0)     close(st->spi_fd);
    if (st->gpiochip_fd >= 0) close(st->gpiochip_fd);
}

static const struct lora_transport_vtable PI_VTABLE = {
    pi_init, pi_send, pi_recv, pi_rssi, pi_close, pi_get_irq_fd
};

/* Exported constructor used by the Pi node app. */
void lora_transport_init_pi(lora_transport_t *t, pi_lora_state_t *st)
{
    t->vtable = &PI_VTABLE;
    t->impl   = st;
    memset(st, 0, sizeof(*st));
    st->spi_fd = -1;
    st->gpiochip_fd = -1;
}