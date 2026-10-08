# solo1's portable CTAP2/U2F core, plus the crypto and CBOR libraries it uses.
# Shared by the native build (solo_core.cmake) and the ESP-IDF component.
# Ports supply the device hooks (transport, presence, storage, rng) by
# overriding the core's weak defaults.
set(TINYCRYPT_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(SOLO1 ${TINYCRYPT_ROOT}/third_party/solo1)

if(NOT EXISTS ${SOLO1}/tinycbor/src/cbor.h)
  message(FATAL_ERROR "solo1 submodules missing: run 'git submodule update --init --recursive'")
endif()

set(SOLO_CORE_SOURCES
  ${SOLO1}/fido2/apdu.c
  ${SOLO1}/fido2/util.c
  ${SOLO1}/fido2/u2f.c
  ${SOLO1}/fido2/test_power.c
  ${SOLO1}/fido2/stubs.c
  ${SOLO1}/fido2/log.c
  ${SOLO1}/fido2/ctaphid.c
  ${SOLO1}/fido2/ctap.c
  ${SOLO1}/fido2/ctap_parse.c
  ${SOLO1}/fido2/crypto.c
  ${SOLO1}/fido2/device.c
  ${SOLO1}/fido2/version.c
  ${SOLO1}/fido2/data_migration.c
  ${SOLO1}/fido2/extensions/extensions.c
  ${SOLO1}/fido2/extensions/solo.c
  ${SOLO1}/fido2/extensions/wallet.c
  ${SOLO1}/crypto/sha256/sha256.c
  ${SOLO1}/crypto/micro-ecc/uECC.c
  ${SOLO1}/crypto/tiny-AES-c/aes.c
  ${SOLO1}/crypto/cifra/src/sha512.c
  ${SOLO1}/crypto/cifra/src/blockwise.c
  ${SOLO1}/tinycbor/src/cborencoder.c
  ${SOLO1}/tinycbor/src/cborencoder_close_container_checked.c
  ${SOLO1}/tinycbor/src/cborerrorstrings.c
  ${SOLO1}/tinycbor/src/cborparser.c
  ${SOLO1}/tinycbor/src/cborparser_dup_string.c
  ${SOLO1}/tinycbor/src/cborpretty.c
  ${SOLO1}/tinycbor/src/cborpretty_stdio.c
  ${SOLO1}/tinycbor/src/cborvalidation.c
)

# tinycrypt's own portable code (CC0), shared by the sim and the ESP32 build.
set(TINYCRYPT_PORT_SOURCES
  ${TINYCRYPT_ROOT}/port/presence_sig.c
)

set(SOLO_CORE_INCLUDES
  ${TINYCRYPT_ROOT}/port
  ${SOLO1}/fido2
  ${SOLO1}/fido2/extensions
  ${SOLO1}/tinycbor/src
  ${SOLO1}/crypto/sha256
  ${SOLO1}/crypto/micro-ecc
  ${SOLO1}/crypto/tiny-AES-c
  ${SOLO1}/crypto/cifra/src
  ${SOLO1}/crypto/cifra/src/ext
)

set(SOLO_CORE_DEFINES
  APP_CONFIG="tinycrypt_app.h"
  AES256=1
  SOLO_VERSION_MAJ=0
  SOLO_VERSION_MIN=1
  SOLO_VERSION_PATCH=0
  SOLO_VERSION="tinycrypt-spike"
)
if(TINYCRYPT_INSECURE_SOFT_KEY)
  list(APPEND SOLO_CORE_DEFINES TINYCRYPT_INSECURE_SOFT_KEY)
endif()
if(TINYCRYPT_RELEASE)
  list(APPEND SOLO_CORE_DEFINES TINYCRYPT_RELEASE)
endif()
