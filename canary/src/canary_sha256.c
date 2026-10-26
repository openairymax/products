/* SPDX-FileCopyrightText: 2025-2026 SPHARX Ltd. */
/* SPDX-License-Identifier: AGPL-3.0-or-later OR Apache-2.0 */

/*
 * canary_sha256.c - FIPS 180-4 SHA-256 紧凑实现。
 *
 * manifest base 哈希（§7.0.2.3）依赖：agentrt 全域无现成 SHA-256，
 * 本单元零依赖自持。实现为标准规范直译，仅少分配、无查找表展开。
 */
#include "canary_sha256.h"

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static uint32_t be32dec(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void be32enc(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void compress(uint32_t h[8], const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, hh;
    uint32_t s0, s1, t1, t2;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = be32dec(block + i * 4);
    for (i = 16; i < 64; i++) {
        s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0];
    b = h[1];
    cc = h[2];
    d = h[3];
    e = h[4];
    f = h[5];
    g = h[6];
    hh = h[7];
    for (i = 0; i < 64; i++) {
        s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        t1 = hh + s1 + ((e & f) ^ (~e & g)) + K[i] + w[i];
        s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        t2 = s0 + ((a & b) ^ (a & cc) ^ (b & cc));
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = cc;
        cc = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += cc;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

void sha256_init(sha256_ctx *ctx)
{
    static const uint32_t IV[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };
    int i;

    for (i = 0; i < 8; i++)
        ctx->h[i] = IV[i];
    ctx->len = 0;
    ctx->fill = 0;
}

void sha256_update(sha256_ctx *ctx, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t i;

    ctx->len += (uint64_t)len;
    if (ctx->fill > 0) {
        size_t take = 64 - ctx->fill;

        if (take > len)
            take = len;
        for (i = 0; i < take; i++)
            ctx->buf[ctx->fill + i] = p[i];
        ctx->fill += take;
        p += take;
        len -= take;
        if (ctx->fill == 64) {
            compress(ctx->h, ctx->buf);
            ctx->fill = 0;
        }
    }
    while (len >= 64) {
        compress(ctx->h, p);
        p += 64;
        len -= 64;
    }
    if (len > 0) {
        for (i = 0; i < len; i++)
            ctx->buf[i] = p[i];
        ctx->fill = len;
    }
}

void sha256_final(sha256_ctx *ctx, uint8_t out[32])
{
    uint64_t bits = ctx->len * 8ull;
    uint8_t pad = 0x80u;
    uint8_t tail[8];
    int i;

    sha256_update(ctx, &pad, 1);
    pad = 0x00u;
    while (ctx->fill != 56)
        sha256_update(ctx, &pad, 1);
    for (i = 0; i < 8; i++)
        tail[i] = (uint8_t)(bits >> (56 - i * 8));
    sha256_update(ctx, tail, 8);
    for (i = 0; i < 8; i++)
        be32enc(out + i * 4, ctx->h[i]);
}

void sha256_buf(const void *data, size_t len, uint8_t out[32])
{
    sha256_ctx ctx;

    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

void sha256_hex(const uint8_t digest[32], char out[65])
{
    static const char HEX[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 32; i++) {
        out[i * 2] = HEX[digest[i] >> 4];
        out[i * 2 + 1] = HEX[digest[i] & 0x0fu];
    }
    out[64] = '\0';
}
