# ESP-IDF main component: the node app for ESP32-S3.

set(COMPONENT_SRCS
    "../../node/main_esp32.c"
    "../../node/forwarding.c"
)

idf_component_register(
    SRCS ${COMPONENT_SRCS}
    INCLUDE_DIRS "../../node" "../../dtn_core" "../../transport"
    REQUIRES dtn_core dtn_transport driver
)