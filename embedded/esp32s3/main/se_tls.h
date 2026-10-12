// TLS client authentication with the secure element's IoT key (se_tls.c).
#pragma once

// A PEM key for esp-mqtt whose signatures are made by the secure element (SE_SLOT_IOT). It holds
// no secret. malloc'd; NULL on failure.
char *se_tls_key_pem(void);
