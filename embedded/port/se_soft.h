// Storage for the INSECURE software secure element (se_soft.c), supplied by each port: a file in
// the sim's state directory, a blob in NVS on the ESP32.
#ifndef TINYCRYPT_SE_SOFT_H
#define TINYCRYPT_SE_SOFT_H

#include <stdbool.h>
#include <stddef.h>

// 0 when exactly len bytes were read.
int se_soft_store_load(void *buf, size_t len);
// 0 when written durably.
int se_soft_store_save(const void *buf, size_t len);
// Whether anything is stored, readable or not.
bool se_soft_store_exists(void);

#endif
