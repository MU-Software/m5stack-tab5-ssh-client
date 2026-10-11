/* SPDX-License-Identifier: GPL-3.0-or-later
 * Derived from wolfSSL's Espressif user_settings.h, Copyright (C) 2006-2026 wolfSSL Inc. */
#pragma once

#define WOLFCRYPT_ONLY
#define NO_FILESYSTEM
#define NO_OLD_TLS
#define NO_DSA
#define NO_DES3
#define NO_MD5
#define NO_RC4
#define NO_PSK
#define NO_PWDBASED

#define WOLFSSL_SHA384
#define WOLFSSL_SHA512
#define HAVE_AESGCM
#define WOLFSSL_AES_COUNTER
#define WOLFSSL_AES_DIRECT
#define HAVE_AEAD
#define HAVE_HKDF

#define HAVE_ECC
#define ECC_TIMING_RESISTANT
#define HAVE_CURVE25519
#define HAVE_ED25519
#define WOLFSSL_ED25519_STREAMING_VERIFY

#define WC_RSA_BLINDING
#define WC_RSA_PSS
#define RSA_LOW_MEM
#define WOLFSSL_KEY_GEN

#define WOLFSSL_SP_MATH_ALL
#define WOLFSSL_SP_SMALL
/* RSA-4096 host keys fail to verify (WS_RSA_E) without it */
#define WOLFSSL_SP_4096
#define TFM_TIMING_RESISTANT
#define WOLFSSL_SMALL_STACK
#define WOLFSSL_ASN_TEMPLATE

#define WOLFSSL_WOLFSSH
#define WOLFSSH_TERM
/* Modes come from wolfSSH_SetTerminal; the USB-Serial-JTAG console's termios has ISIG off and VINTR 0 */
#define NO_TERMIOS
#define WOLFSSH_USER_IO
#define WOLFSSH_SMALL_STACK

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <sdkconfig.h>

#undef WOLFSSL_ESPIDF
#define WOLFSSL_ESPIDF
#undef WOLFSSL_ESP32
#define WOLFSSL_ESP32

/* wolfSSL has no ESP32-P4 hardware acceleration */
#define NO_ESP32_CRYPT
#define NO_WOLFSSL_ESP32_CRYPT_HASH
#define NO_WOLFSSL_ESP32_CRYPT_AES
#define NO_WOLFSSL_ESP32_CRYPT_RSA_PRI

/* Internal RAM by default; the channel window alone is 32-128 KiB */
#define NO_WOLFSSL_MEMORY
#define XMALLOC_OVERRIDE
#define WOLF_PSRAM_CAPS      (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define WOLF_INTERNAL_CAPS   (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define XMALLOC(s, h, t)     ((void)(h), (void)(t), heap_caps_malloc_prefer((s), 2, WOLF_PSRAM_CAPS, WOLF_INTERNAL_CAPS))
#define XFREE(p, h, t)       ((void)(h), (void)(t), heap_caps_free(p))
#define XREALLOC(p, n, h, t) ((void)(h), (void)(t), heap_caps_realloc_prefer((p), (n), 2, WOLF_PSRAM_CAPS, WOLF_INTERNAL_CAPS))
#else
/* NO_FILESYSTEM also turns off /dev/urandom seeding */
#include <sys/random.h>
#define CUSTOM_RAND_GENERATE_SEED(out, sz) getentropy((out), (sz))
#endif
