# Hardware — DTN Prototype

## Parts list

### Pi 4 nodes (×2: Earth Ground `EART`, Earth Relay `ERLY`)
- Raspberry Pi 4 (2 GB+ RAM) + microSD + 5 V/3 A PSU
- Waveshare SX127x LoRa HAT (or Dragino LoRa/GPS HAT) — 868 MHz variant
- (Optional) small OLED/MAX6675 for status; not required

### ESP32-S3 nodes (×3: Mars Relay `MRLY`, Mars Lander `MLND`, Rover `ROVR`)
- ESP32-S3-DevKitC-1 (with PSRAM, e.g. ESP32-S3-WROOM-1 N16R8)
- RFM95W breakout (Adafruit #3072 style, 868 MHz) — one per node
- Breadboard + jumper wires + 3.3 V supply (USB power is fine)

### Misc
- 868 MHz antennas (proper SMA/SMA-F ones matched to the LoRa board) — **benchtop low power only**
- USB-to-TTL serial adapter (for ESP32 monitor logs if not using USB-CDC)
- microSD cards (Pi) — 16 GB class 10

## Wiring

### Pi 4 ↔ RFM95 HAT (SPI0)
| RFM95 pin | Pi 4 GPIO | Function |
|-----------|-----------|----------|
| VCC       | 3.3 V     | power (do NOT use 5 V) |
| GND       | GND       | ground |
| MISO      | GPIO9     | SPI0 MISO |
| MOSI      | GPIO10    | SPI0 MOSI |
| SCK       | GPIO11    | SPI0 SCLK |
| NSS/CS    | GPIO8     | SPI0 CE0 (`/dev/spidev0.0`) |
| RESET     | GPIO22    | radio reset |
| DIO0      | GPIO25    | RX_DONE / TX_DONE IRQ |
| DIO1      | GPIO24    | (optional) CAD detection |

Enable SPI on the Pi: `sudo raspi-config` → Interface Options → SPI → enable.

### ESP32-S3 ↔ RFM95 (SPI, default pins)
| RFM95 pin | ESP32-S3 GPIO | Function |
|-----------|---------------|----------|
| VCC       | 3V3           | power |
| GND       | GND           | ground |
| MISO      | GPIO13        | SPI MISO |
| MOSI      | GPIO11        | SPI MOSI |
| SCK       | GPIO12        | SPI SCLK |
| NSS/CS    | GPIO10        | SPI CS |
| RESET     | GPIO5         | radio reset |
| DIO0      | GPIO6         | RX_DONE / TX_DONE IRQ (interrupt) |
| DIO1      | GPIO7         | (optional) CAD detection |

## RF safety / legal

- 868 MHz band, EU. Duty cycle limit is 1% for most sub-bands. The contact plan
  uses ~60 s windows every 5 min (~20% per link) — fine for a benchtop,
  low-power, educational demo with no or stub antennas. Do **not** attach
  high-gain antennas or raise TX power above the regulatory EIRP limit
  (+14 dBm in the relevant EU sub-band). Keep the boards on the bench,
  no antennas attached, for the first bring-up.
- If you're outside the EU, change the frequency in `lora_config_t.freq_mhz`
  to your local band (915 MHz US, etc.) and respect local EIRP/duty-cycle limits.

## Time sync

Scheduled contacts need a shared clock.
- Pi 4: sync via NTP at boot.
- ESP32-S3: no on-board RTC. MVP convention: boot time = epoch 0 for all nodes
  (start them together). M5+ will add SNTP over Wi-Fi or a LoRa time-push
  from the ground station at boot.