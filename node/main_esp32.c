/* node/main_esp32.c — DTN node firmware for the ESP32-S3.
 *
 * FreeRTOS tasks:
 *   - radio_rx_task: blocks on the LoRa DIO0 semaphore, calls
 *     node_handle_received_frame on each frame.
 *   - dtn_tick_task: 100 ms periodic, calls node_tick.
 *   - cli_task (optional, USB-CDC): accepts commands over the serial
 *     console. Enabled when CONFIG_DTN_ENABLE_CLI is set in sdkconfig.
 *
 * The contact plan is flashed into a LittleFS partition at /spiffs/contact_plan.json.
 */
#ifdef ESP_PLATFORM

#include "forwarding.h"
#include "../transport/lora_transport.h"
#include "../dtn_core/contact.h"
#include "../dtn_core/trace.h"
#include "../dtn_core/types.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "dtn-node";

/* --- Per-node config (set at build time via menuconfig or sdkconfig.defaults) --- */
#ifndef CONFIG_DTN_NODE_ID
#define CONFIG_DTN_NODE_ID "MRLY"
#endif
#ifndef CONFIG_DTN_PLAN_PATH
#define CONFIG_DTN_PLAN_PATH "/spiffs/contact_plan.json"
#endif
#ifndef CONFIG_DTN_BUFFER_CAP
#define CONFIG_DTN_BUFFER_CAP 4096.0f
#endif

static node_state_t g_ns;
static lora_transport_t g_radio;
static esp_lora_state_t g_lora_state;
static contact_plan_t g_plan;
static volatile BaseType_t g_running = 1;

/* Trace sink: log via ESP_LOG. */
static void esp_trace_sink(const trace_record_t *rec, void *user)
{
    (void)user;
    char node_str[5]; node_id_to_str(rec->node, node_str);
    ESP_LOGI(TAG, "[%.2f] %s %s b=%llu %s",
             (double)rec->t, node_str, trace_event_name(rec->event),
             (unsigned long long)rec->bundle_id, rec->detail);
}

static dtn_time_t wall_clock_seconds(void)
{
    /* ESP32-S3 has no RTC battery; use tick count as a monotonic clock.
     * The ground station broadcasts time-sync messages over LoRa at boot;
     * node_try_handle_time_sync sets ns->time_offset so all nodes share
     * the same epoch. Until the first sync arrives, local boot time is
     * the epoch (good enough for MVP where nodes start together). */
    return (dtn_time_t)(xTaskGetTickCount() / (double)configTICK_RATE_HZ)
           + g_ns.time_offset;
}

/* --- Tasks --- */

static void radio_rx_task(void *arg)
{
    node_state_t *ns = (node_state_t *)arg;
    uint8_t buf[256];
    while (g_running) {
        int n = lora_recv(ns->radio, buf, sizeof(buf), 5000);
        if (n > 0) {
            dtn_time_t t = wall_clock_seconds();
            /* Check if it's a time-sync frame first. */
            int ts = node_try_handle_time_sync(ns, t, buf, (size_t)n);
            if (ts == 0) {
                /* Not a time-sync frame — handle as a bundle. */
                node_handle_received_frame(ns, t, buf, (size_t)n);
            }
        }
    }
    vTaskDelete(NULL);
}

static void dtn_tick_task(void *arg)
{
    node_state_t *ns = (node_state_t *)arg;
    const TickType_t period = pdMS_TO_TICKS(100);
    TickType_t last = xTaskGetTickCount();
    while (g_running) {
        vTaskDelayUntil(&last, period);
        dtn_time_t t = wall_clock_seconds();
        node_tick(ns, t);
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "DTN node starting: id=%s", CONFIG_DTN_NODE_ID);

    /* NVS (required by some ESP-IDF components). */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Load contact plan. */
    int n = contact_plan_load_json(&g_plan, CONFIG_DTN_PLAN_PATH);
    if (n <= 0) {
        ESP_LOGW(TAG, "no contact plan loaded from %s (running with empty plan)",
                 CONFIG_DTN_PLAN_PATH);
    } else {
        ESP_LOGI(TAG, "loaded %d contacts", n);
    }

    /* LoRa transport. */
    lora_config_t cfg;
    lora_config_default_868(&cfg, /*cs*/ 10, /*reset*/ 5, /*dio0*/ 6, /*dio1*/ 7);
    lora_transport_init_esp32(&g_radio, &g_lora_state);
    if (lora_init(&g_radio, &cfg) != 0) {
        ESP_LOGE(TAG, "LoRa init failed");
    }

    /* Node state. */
    node_state_init(&g_ns, node_id_from_str(CONFIG_DTN_NODE_ID),
                    CONFIG_DTN_BUFFER_CAP, "accept", &g_radio,
                    esp_trace_sink, NULL);
    g_ns.plan = g_plan;

    /* Spawn tasks. */
    xTaskCreate(radio_rx_task,  "radio_rx", 4096, &g_ns, 5, NULL);
    xTaskCreate(dtn_tick_task,   "dtn_tick", 4096, &g_ns, 4, NULL);

    ESP_LOGI(TAG, "node up; tasks started");
    /* app_main returns; the FreeRTOS scheduler keeps the tasks running. */
}

#endif /* ESP_PLATFORM */