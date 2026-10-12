// The ATECC608B backend's extra entry point (se_atecc.c), for the provisioner.
#pragma once

// Start cryptoauthlib on the I2C bus, without the checks se_init makes (the provisioner talks to
// blank chips). 0 on success.
int se_atecc_open(void);
