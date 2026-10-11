# tinycrypt firmware for the ESP32-S3: the solo1 core behind TinyUSB FIDO HID.
#
#   . ~/esp/v5.5/esp-idf/export.sh
#   idf.py -DTINYCRYPT_INSECURE_SOFT_KEY=ON build flash
cmake_minimum_required(VERSION 3.16)

option(TINYCRYPT_INSECURE_SOFT_KEY "INSECURE software keystore - TEST ONLY" OFF)
option(TINYCRYPT_RELEASE "Release configuration (refuses the software keystore)" OFF)
if(TINYCRYPT_RELEASE AND TINYCRYPT_INSECURE_SOFT_KEY)
  message(FATAL_ERROR "TINYCRYPT_INSECURE_SOFT_KEY must never be built in a release configuration")
endif()

set(IDF_TARGET esp32s3)
# Every key runs the same image: per-key settings, the Wi-Fi password and the IoT certificate are in
# the prov partition (infra/scripts/provision-key.sh), never in the build (CRYPT-14).
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tinycrypt)
