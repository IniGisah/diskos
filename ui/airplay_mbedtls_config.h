/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DISKOS_AIRPLAY_MBEDTLS_CONFIG_H
#define DISKOS_AIRPLAY_MBEDTLS_CONFIG_H
/* Legacy RAOP specifies RSA-OAEP/SHA-1 and AES-128-CBC. SHA-1 is used for
 * that protocol's key envelope, not to relax TLS certificate policy. */
#define MBEDTLS_BASE64_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_CIPHER_MODE_CBC
#include "diskos_mbedtls_config.h"
#endif
