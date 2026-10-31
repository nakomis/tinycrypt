# tinycrypt firmware for the ESP32-S3: the solo1 core behind TinyUSB FIDO HID.
#
#   . ~/esp/v5.5/esp-idf/export.sh
#   esp32s3/build.sh                       (soft key) or  KEYSTORE=atecc esp32s3/build.sh
cmake_minimum_required(VERSION 3.16)

# The keystore (CRYPT-22): the ATECC608B, or the INSECURE software stand-in. Exactly one.
option(TINYCRYPT_KEYSTORE_ATECC "Secure element: ATECC608B" OFF)
option(TINYCRYPT_INSECURE_SOFT_KEY "INSECURE software keystore - TEST ONLY" OFF)
option(TINYCRYPT_RELEASE "Release configuration (refuses the software keystore)" OFF)
if(TINYCRYPT_RELEASE AND TINYCRYPT_INSECURE_SOFT_KEY)
  message(FATAL_ERROR "TINYCRYPT_INSECURE_SOFT_KEY must never be built in a release configuration")
endif()
if(TINYCRYPT_KEYSTORE_ATECC AND TINYCRYPT_INSECURE_SOFT_KEY)
  message(FATAL_ERROR "Choose one keystore: TINYCRYPT_KEYSTORE_ATECC or TINYCRYPT_INSECURE_SOFT_KEY")
endif()
if(NOT TINYCRYPT_KEYSTORE_ATECC AND NOT TINYCRYPT_INSECURE_SOFT_KEY)
  message(FATAL_ERROR "No keystore chosen: -DTINYCRYPT_KEYSTORE_ATECC=ON (or -DTINYCRYPT_INSECURE_SOFT_KEY=ON for testing)")
endif()

set(IDF_TARGET esp32s3)
# Every key runs the same image: per-key settings, the Wi-Fi password and the IoT certificate are in
# the prov partition (infra/scripts/provision-key.sh), never in the build (CRYPT-14).
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The variant (CRYPT-34): plain, or secured (sdkconfig.secured.defaults: secure boot, flash and NVS
# encryption). It's the image's project name, which Fleet's firmwareProject and the OTA check go by,
# so a key only ever installs its own variant.
set(TINYCRYPT_VARIANT "plain" CACHE STRING "plain or secured")
if(TINYCRYPT_VARIANT STREQUAL "plain")
  project(tinycrypt)
elseif(TINYCRYPT_VARIANT STREQUAL "secured")
  project(tinycrypt-secured)
else()
  message(FATAL_ERROR "TINYCRYPT_VARIANT must be plain or secured")
endif()
