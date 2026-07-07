/* transport/lora_esp32.c — ESP-IDF LoRa driver for RFM95/SX1276 on ESP32-S3.
 *
 * Uses the ESP-IDF SPI master driver and a GPIO interrupt on DIO0 for
 * RX_DONE / TX_DONE. A FreeRTOS task waits on a binary semaphore that the
 * ISR gives on edge events.
 *
 * NOTE: this file is compiled only under ESP-IDF; the Pi build excludes it.
 * It uses <driver/spi_master.h>, <driver/gpio.h>, and FreeRTOS headers.
 */
#ifdef ESP_PLATFORM

#include "lora_transport.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>
#include <unistd.h>

/* SX1276 register map (same as lora_pi.c). */
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
#define REG_PKT_RSSI_VALUE       0x1A
#define REG_MODEM_CONFIG_1       0x1D
#define REG_MODEM_CONFIG_2       0x1E
#define REG_PREAMBLE_MSB         0x20
#define REG_PREAMBLE_LSB         0x21
#define REG_PAYLOAD_LENGTH        0x22
#define REG_SYNC_WORD            0x39
#define REG_LNA                   0x0C
#define REG_MODEM_CONFIG_3       0x26
#define REG_SYMBOL_TIMEOUT_MSB    0x1F

#define MODE_SLEEP 0x00
#define MODE_STDBY 0x01
#define MODE_TX    0x03
#define MODE_RX_CONT 0x05
#define MODE_RX_SINGLE 0x06
#define MODE_LORA  0x80

#define IRQ_TX_DONE_MASK 0x08
#define IRQ_RX_DONE_MASK 0x40
#define IRQ_PAYLOAD_CRC_ERR 0x20

#define PIN_NUM_MISO 13
#define PIN_NUM_MOSI 11
#define PIN_NUM_SCLK 12
#define PIN_NUM_CS   10

/* esp_lora_state struct is defined in lora_transport.h (under #ifdef ESP_PLATFORM). */

static const char *TAG = "lora_esp32";

static uint8_t reg_read(spi_device_handle_t spi, uint8_t addr)
{
    uint8_t tx[2] = { (uint8_t)(addr & 0x7F), 0x00 };
    uint8_t rx[2] = { 0 };
    spi_transaction_t t = { 0 };
    t.length    = 16;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    spi_device_polling_transmit(spi, &t);
    return rx[1];
}

static void reg_write(spi_device_handle_t spi, uint8_t addr, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)(addr | 0x80), val };
    spi_transaction_t t = { 0 };
    t.length    = 16;
    t.tx_buffer = tx;
    spi_device_polling_transmit(spi, &t);
}

static void set_mode(esp_lora_state_t *st, uint8_t mode)
{
    reg_write(st->spi, REG_OP_MODE, (uint8_t)(MODE_LORA | mode));
}

static void IRAM_ATTR dio0_isr(void *arg)
{
    esp_lora_state_t *st = (esp_lora_state_t *)arg;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(st->dio0_sem, &hp);
    if (hp) portYIELD_FROM_ISR();
}

/* --- vtable --- */

static int esp_init(lora_transport_t *t, const lora_config_t *cfg)
{
    esp_lora_state_t *st = (esp_lora_state_t *)t->impl;
    if (!st) return -1;
    st->cs_pin    = cfg->cs_pin    > 0 ? cfg->cs_pin    : PIN_NUM_CS;
    st->reset_pin = cfg->reset_pin > 0 ? cfg->reset_pin : 5;
    st->dio0_pin  = cfg->dio0_pin  > 0 ? cfg->dio0_pin  : 6;

    /* SPI bus setup. */
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256,
    };
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 8 * 1000 * 1000,   /* 8 MHz */
        .mode = 0,
        .spics_io_num = st->cs_pin,
        .queue_size = 4,
    };
    if (spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed");
        return -1;
    }
    if (spi_bus_add_device(SPI2_HOST, &devcfg, &st->spi) != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed");
        return -1;
    }

    /* Reset pulse. */
    gpio_set_direction(st->reset_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(st->reset_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(st->reset_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* DIO0 interrupt. */
    st->dio0_sem = xSemaphoreCreateBinary();
    gpio_set_direction(st->dio0_pin, GPIO_MODE_INPUT);
    gpio_set_intr_type(st->dio0_pin, GPIO_INTR_POSEDGE);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(st->dio0_pin, dio0_isr, st);

    /* Sleep -> configure. */
    set_mode(st, MODE_SLEEP);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Frequency. */
    uint32_t frf = (uint32_t)(cfg->freq_mhz * (1u << 19) / 32.0f);
    reg_write(st->spi, REG_FRF_MSB, (uint8_t)(frf >> 16));
    reg_write(st->spi, REG_FRF_MID, (uint8_t)(frf >> 8));
    reg_write(st->spi, REG_FRF_LSB, (uint8_t)frf);

    reg_write(st->spi, REG_PA_CONFIG, (uint8_t)(0x80 | (cfg->tx_power_dbm & 0x0F)));
    reg_write(st->spi, REG_LNA, 0x23);

    uint8_t bw_code = 0x07; /* 125 kHz */
    uint8_t cr_code = (uint8_t)((cfg->coding_rate - 4) & 0x07);
    reg_write(st->spi, REG_MODEM_CONFIG_1, (uint8_t)((bw_code << 4) | (cr_code << 1)));
    reg_write(st->spi, REG_MODEM_CONFIG_2, (uint8_t)((cfg->sf << 4) | 0x04));
    reg_write(st->spi, REG_SYMBOL_TIMEOUT_MSB, 0x00);
    reg_write(st->spi, REG_MODEM_CONFIG_3, 0x04);   /* AGC auto on */

    reg_write(st->spi, REG_PREAMBLE_MSB, (uint8_t)((cfg->preamble_len >> 8) & 0xFF));
    reg_write(st->spi, REG_PREAMBLE_LSB, (uint8_t)(cfg->preamble_len & 0xFF));
    reg_write(st->spi, REG_SYNC_WORD, (uint8_t)cfg->sync_word);
    reg_write(st->spi, REG_FIFO_TX_BASE, 0x00);
    reg_write(st->spi, REG_FIFO_RX_BASE, 0x00);

    set_mode(st, MODE_STDBY);
    return 0;
}

static int esp_send(lora_transport_t *t, const uint8_t *buf, size_t len)
{
    esp_lora_state_t *st = (esp_lora_state_t *)t->impl;
    if (len > 222) return -1;
    set_mode(st, MODE_STDBY);
    reg_write(st->spi, REG_FIFO_ADDR_PTR, 0x00);
    for (size_t i = 0; i < len; ++i)
        reg_write(st->spi, REG_FIFO, buf[i]);
    reg_write(st->spi, REG_PAYLOAD_LENGTH, (uint8_t)len);
    reg_write(st->spi, REG_IRQ_MASK, 0x00);
    set_mode(st, MODE_TX);

    /* Wait on DIO0 semaphore (TX_DONE). */
    if (xSemaphoreTake(st->dio0_sem, pdMS_TO_TICKS(2000)) == pdTRUE) {
        reg_write(st->spi, REG_IRQ_FLAGS, 0xFF);
        set_mode(st, MODE_STDBY);
        return (int)len;
    }
    set_mode(st, MODE_STDBY);
    return -1;
}

static int esp_recv(lora_transport_t *t, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    esp_lora_state_t *st = (esp_lora_state_t *)t->impl;
    set_mode(st, MODE_STDBY);
    reg_write(st->spi, REG_IRQ_MASK, 0x00);
    set_mode(st, MODE_RX_SINGLE);

    if (xSemaphoreTake(st->dio0_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        set_mode(st, MODE_STDBY);
        return 0;   /* timeout */
    }
    uint8_t irq = reg_read(st->spi, REG_IRQ_FLAGS);
    reg_write(st->spi, REG_IRQ_FLAGS, 0xFF);
    if (irq & IRQ_PAYLOAD_CRC_ERR) {
        set_mode(st, MODE_STDBY);
        return -1;
    }
    if (!(irq & IRQ_RX_DONE_MASK)) {
        set_mode(st, MODE_STDBY);
        return 0;
    }
    uint8_t rx_len = reg_read(st->spi, REG_RX_NB_BYTES);
    if (rx_len > cap) rx_len = (uint8_t)cap;
    uint8_t fifo_addr = reg_read(st->spi, REG_FIFO_RX_CURR);
    reg_write(st->spi, REG_FIFO_ADDR_PTR, fifo_addr);
    for (uint8_t i = 0; i < rx_len; ++i)
        buf[i] = reg_read(st->spi, REG_FIFO);
    set_mode(st, MODE_STDBY);
    return rx_len;
}

static int esp_rssi(lora_transport_t *t)
{
    esp_lora_state_t *st = (esp_lora_state_t *)t->impl;
    return (int)reg_read(st->spi, REG_PKT_RSSI_VALUE) - 157;
}

static void esp_close(lora_transport_t *t)
{
    esp_lora_state_t *st = (esp_lora_state_t *)t->impl;
    set_mode(st, MODE_SLEEP);
    /* Bus teardown omitted for brevity; the device is typically reset on reboot. */
}

static const struct lora_transport_vtable ESP_VTABLE = {
    esp_init, esp_send, esp_recv, esp_rssi, esp_close,
    /* get_irq_fd: ESP32 uses a FreeRTOS semaphore, not a fd. Return -1
     * so callers know there's no poll()-able fd. The ESP32 node app
     * drives RX via the semaphore in a dedicated task. */
    NULL
};

void lora_transport_init_esp32(lora_transport_t *t, esp_lora_state_t *st)
{
    t->vtable = &ESP_VTABLE;
    t->impl   = st;
    memset(st, 0, sizeof(*st));
}

#endif /* ESP_PLATFORM */