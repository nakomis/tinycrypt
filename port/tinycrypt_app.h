// tinycrypt app config for the solo1 CTAP2 core (passed as APP_CONFIG).
// Shared by every port; port-specific settings belong in the port itself.
#ifndef TINYCRYPT_APP_H
#define TINYCRYPT_APP_H

#include <stdbool.h>

#include "tinycrypt_keystore.h"

#define DEBUG_LEVEL 1

// CTAP1/U2F fallback on, Solo's vendor extensions (wallet, bootloader) off.
#define ENABLE_U2F

#endif
