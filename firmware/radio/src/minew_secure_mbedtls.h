/* SPDX-License-Identifier: MIT */
#ifndef MINEW_SECURE_MBEDTLS_H
#define MINEW_SECURE_MBEDTLS_H
/* Cookie validation only starts a new handshake. Application state must remain
 * unauthenticated until that handshake verifies the configured pair identity. */
#define MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#endif
