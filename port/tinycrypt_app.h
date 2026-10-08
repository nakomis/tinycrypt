// tinycrypt app config for the solo1 CTAP2 core (passed as APP_CONFIG).
// Shared by every port; port-specific settings belong in the port itself.
#ifndef TINYCRYPT_APP_H
#define TINYCRYPT_APP_H

#include <stdbool.h>

#include "tinycrypt_keystore.h"

#define DEBUG_LEVEL 1

// CTAP1/U2F fallback on, Solo's vendor extensions (wallet, bootloader) off.
#define ENABLE_U2F

// tinycrypt's own AAGUID (random v4: f3751ba0-8b97-47ac-aba7-d1c15c36fecf).
// solo1's default is listed in the FIDO metadata service as "HYPR FIDO2
// Authenticator", so relying parties showed our key under HYPR's name.
#define TINYCRYPT_AAGUID "\xf3\x75\x1b\xa0\x8b\x97\x47\xac\xab\xa7\xd1\xc1\x5c\x36\xfe\xcf"

#endif
