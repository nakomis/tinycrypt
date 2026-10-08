#pragma once

#include <stdbool.h>

// Verifies the committed Secure Enclave vector with micro-ecc and mbedTLS and
// logs one "se-vector:" line. Returns true if both accept it.
bool se_selftest_run(void);
