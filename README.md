# DTN-Hardware

A real, working Delay-Tolerant Network (DTN) prototype on physical hardware —
Pi 4 nodes and ESP32-S3 nodes, all linked by LoRa (RFM95/SX1276) at 868 MHz.
Bundles store-and-forward across scheduled contact windows with custody
transfer, the same model as the Python simulator in `../Project_DSN`, but
running on real radios with real propagation and real intermittent contacts.

## Status

Code-first build: the DTN core library, transport interface, node apps,
and CLI are written against the driver interface so you can flash the
firmware onto boards as they arrive. See the milestones in `plan.md`.

## Repository layout

```
dtn-hardware/
├── README.md
├── HARDWARE.md              # wiring, RF safety, parts list
├── CMakeLists.txt            # root (Pi build)
├── dtn_core/                # shared C99 library (Pi + ESP32-S3)
├── transport/               # LoRa transport: lora_pi.c, lora_esp32.c
├── node/                     # main_pi.c (Linux), main_esp32.c (ESP-IDF)
├── ground/                   # dtn-cli + Flask dashboard
├── config/                   # contact_plan.json
└── esp32-firmware/           # ESP-IDF project
```

## Nodes (5)

| Role            | Board      | Node ID |
|-----------------|------------|---------|
| Earth Ground    | Pi 4       | `EART`  |
| Earth Relay     | Pi 4       | `ERLY`  |
| Mars Relay      | ESP32-S3   | `MRLY`  |
| Mars Lander     | ESP32-S3   | `MLND`  |
| Rover           | ESP32-S3   | `ROVR`  |

## Quick start (once hardware is present)

### Pi side
```bash
mkdir build && cd build
cmake ..
make
sudo ./dtn-node --node EART --plan ../config/contact_plan.json
```

### ESP32-S3 side
```bash
cd esp32-firmware
idf.py set-target esp32s3
idf.py -p COM5 flash monitor
```

See `HARDWARE.md` for wiring, RF safety, and the parts list.