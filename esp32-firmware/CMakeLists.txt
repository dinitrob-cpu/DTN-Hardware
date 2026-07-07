# ESP-IDF project for the DTN node firmware on ESP32-S3.
# Builds dtn_core + lora_esp32 + main_esp32 with the ESP-IDF toolchain.
#
# Build:  cd esp32-firmware && idf.py set-target esp32s3 && idf.py build
# Flash:  idf.py -p COM5 flash monitor

cmake_minimum_required(VERSION 3.16)

# Pull in ESP-IDF. The standard idf.py setup provides these macros.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(dtn_esp32 C)

# Add the shared dtn_core as a component so it compiles with the IDF toolchain.
# EXTRA_COMPONENT_DIRS lets us keep the sources where they are.
set(EXTRA_COMPONENT_DIRS "../dtn_core ../transport")

# Component defined inline via the standard IDF component mechanism.
# (See components/ for one-line wrappers around the dtn_core + transport sources.)