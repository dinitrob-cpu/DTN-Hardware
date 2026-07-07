cmake_minimum_required(VERSION 3.16)
project(dtn_hardware C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -O2")

# --- dtn_core static library (shared with ESP32 build) ---
add_library(dtn_core STATIC
    dtn_core/types.c
    dtn_core/bundle.c
    dtn_core/contact.c
    dtn_core/custody.c
    dtn_core/queue.c
    dtn_core/cgr.c
    dtn_core/disruption.c
    dtn_core/trace.c
)
target_include_directories(dtn_core PUBLIC ${CMAKE_SOURCE_DIR})

# --- LoRa transport (Pi side) ---
add_library(dtn_transport_pi STATIC
    transport/lora_common.c
    transport/lora_pi.c
)
target_include_directories(dtn_transport_pi PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(dtn_transport_pi PUBLIC dtn_core)

# lora_esp32.c is excluded here — it's only built under ESP-IDF.
# (The #ifdef ESP_PLATFORM guard makes it a no-op if accidentally included.)

# --- Node app ---
add_executable(dtn-node node/main_pi.c node/forwarding.c)
target_link_libraries(dtn-node dtn_core dtn_transport_pi m)

# --- Unit tests for the core library ---
enable_testing()
add_executable(test_core tests/test_core.c)
target_link_libraries(test_core dtn_core m)
add_test(NAME core_tests COMMAND test_core)

# Install
install(TARGETS dtn-node DESTINATION bin)
install(FILES config/contact_plan.json DESTINATION share/dtn-hardware)