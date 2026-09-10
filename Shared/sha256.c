/**
 ******************************************************************************
 * @file    sha256.c
 * @brief   SHA-256 摘要实现（FIPS 180-4，紧凑实现，无外部依赖）
 ******************************************************************************
 */
#include "sha256.h"
#include <string.h>

/* 64 个轮常量（前 64 个素数的立方根小数部分前 32 位） */
static const uint32_t K[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
    0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
    0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
    0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
    0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

static uint32_t rotr32(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32U - n));
}

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t block[64])
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h, t1, t2;
    uint32_t i;

    for (i = 0; i < 16; i++)
    {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (i = 16; i < 64; i++)
    {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; i++)
    {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);

        t1 = h + S1 + ch + K[i] + w[i];
        t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void sha256_init(sha256_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667UL;
    ctx->state[1] = 0xbb67ae85UL;
    ctx->state[2] = 0x3c6ef372UL;
    ctx->state[3] = 0xa54ff53aUL;
    ctx->state[4] = 0x510e527fUL;
    ctx->state[5] = 0x9b05688cUL;
    ctx->state[6] = 0x1f83d9abUL;
    ctx->state[7] = 0x5be0cd19UL;
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
    while (len > 0)
    {
        uint32_t take = 64U - ctx->buflen;
        if (take > len) take = len;
        memcpy(ctx->buffer + ctx->buflen, data, take);
        ctx->buflen += take;
        ctx->bitlen += (uint64_t)take * 8U;
        data += take;
        len -= take;

        if (ctx->buflen == 64U)
        {
            sha256_transform(ctx, ctx->buffer);
            ctx->buflen = 0;
        }
    }
}

void sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_SIZE])
{
    uint32_t i;
    uint64_t bitlen = ctx->bitlen;
    uint8_t  pad = 0x80;
    uint8_t  zero = 0x00;

    /* 补 0x80 */
    sha256_update(ctx, &pad, 1);
    /* 补 0x00 至长度对 56 取模 */
    while (ctx->buflen != 56U)
    {
        sha256_update(ctx, &zero, 1);
    }
    /* 追加 64 位原始长度（大端） */
    for (i = 0; i < 8; i++)
    {
        uint8_t b = (uint8_t)(bitlen >> (56U - i * 8U));
        sha256_update(ctx, &b, 1);
    }

    for (i = 0; i < 8; i++)
    {
        digest[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void sha256_compute(const uint8_t *data, uint32_t len, uint8_t digest[SHA256_DIGEST_SIZE])
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);
}

int sha256_equal(const uint8_t *a, const uint8_t *b)
{
    uint32_t diff = 0;
    uint32_t i;
    for (i = 0; i < SHA256_DIGEST_SIZE; i++)
    {
        diff |= (uint32_t)(a[i] ^ b[i]);
    }
    return (diff == 0U) ? 1 : 0;
}
