# ESP-IDF component: dtn_transport
# Wraps the LoRa transport (only the ESP32 impl is compiled; lora_pi.c is
# guarded by #ifndef ESP_PLATFORM... actually by #ifdef ESP_PLATFORM in
# lora_esp32.c, and lora_common.c compiles on both).

set(COMPONENT_SRCS
    "../transport/lora_common.c"
    "../transport/lora_esp32.c"
)

idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS "../transport"
    REQUIRES driver
)