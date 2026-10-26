/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * canary_sha256.h - FIPS 180-4 SHA-256 紧凑实现（模块私有）。
 */
#ifndef CANARY_SHA256_H
#define CANARY_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const void *data, size_t len);
void sha256_final(sha256_ctx *ctx, uint8_t out[32]);
void sha256_buf(const void *data, size_t len, uint8_t out[32]);
void sha256_hex(const uint8_t digest[32], char out[65]);

#endif /* CANARY_SHA256_H */
