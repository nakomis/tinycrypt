// Port hooks into port/crypto_tc.c (CRYPT-22).
#ifndef TINYCRYPT_CRYPTO_TC_H
#define TINYCRYPT_CRYPTO_TC_H

// Call from authenticator_read_state() when there is no valid stored state: solo1 is about to make
// a fresh one (no PIN), so the master secret is rolled and no earlier credential carries over.
void crypto_tc_state_missing(void);

#endif
