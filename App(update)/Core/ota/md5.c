/**
 ******************************************************************************
 * @file    md5.c
 * @brief   MD5 摘要实现（RFC 1321），无依赖，栈上使用
 ******************************************************************************
 */
#include "md5.h"
#include <string.h>

/* ---- 每轮位移量 ---- */
static const uint8_t S[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

/* ---- 正弦常数表 K[i] = floor(abs(sin(i+1)) * 2^32) ---- */
static const uint32_t K[64] = {
    0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU,
    0xf57c0fafU, 0x4787c62aU, 0xa8304613U, 0xfd469501U,
    0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU,
    0x6b901122U, 0xfd987193U, 0xa679438eU, 0x49b40821U,
    0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU,
    0xd62f105dU, 0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U,
    0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU,
    0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU,
    0xfffa3942U, 0x8771f681U, 0x6d9d6122U, 0xfde5380cU,
    0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U,
    0x289b7ec6U, 0xeaa127faU, 0xd4ef3085U, 0x04881d05U,
    0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U,
    0xf4292244U, 0x432aff97U, 0xab9423a7U, 0xfc93a039U,
    0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U,
    0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U,
    0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU, 0xeb86d391U
};

static uint32_t rotl32(uint32_t x, uint32_t n)
{
    return (x << n) | (x >> (32U - n));
}

static void md5_block(md5_ctx_t *ctx, const uint8_t *block)
{
    uint32_t M[16];
    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t f, g, temp;
    int i;

    for (i = 0; i < 16; i++)
    {
        M[i] = (uint32_t)block[i * 4] |
               ((uint32_t)block[i * 4 + 1] << 8) |
               ((uint32_t)block[i * 4 + 2] << 16) |
               ((uint32_t)block[i * 4 + 3] << 24);
    }

    for (i = 0; i < 64; i++)
    {
        if (i < 16)
        {
            f = (b & c) | (~b & d);
            g = (uint32_t)i;
        }
        else if (i < 32)
        {
            f = (d & b) | (~d & c);
            g = (5U * (uint32_t)i + 1U) & 15U;
        }
        else if (i < 48)
        {
            f = b ^ c ^ d;
            g = (3U * (uint32_t)i + 5U) & 15U;
        }
        else
        {
            f = c ^ (b | ~d);
            g = (7U * (uint32_t)i) & 15U;
        }

        temp = d;
        d = c;
        c = b;
        b = b + rotl32(a + f + K[i] + M[g], S[i]);
        a = temp;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
}

void md5_init(md5_ctx_t *ctx)
{
    ctx->state[0] = 0x67452301U;
    ctx->state[1] = 0xefcdab89U;
    ctx->state[2] = 0x98badcfeU;
    ctx->state[3] = 0x10325476U;
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

void md5_update(md5_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
    ctx->bitlen += (uint64_t)len * 8U;

    if (ctx->buflen > 0)
    {
        uint32_t take = 64U - ctx->buflen;
        if (take > len)
        {
            take = len;
        }
        memcpy(ctx->buffer + ctx->buflen, data, take);
        ctx->buflen += take;
        data += take;
        len -= take;
        if (ctx->buflen == 64U)
        {
            md5_block(ctx, ctx->buffer);
            ctx->buflen = 0;
        }
    }

    while (len >= 64U)
    {
        md5_block(ctx, data);
        data += 64U;
        len -= 64U;
    }

    if (len > 0)
    {
        memcpy(ctx->buffer, data, len);
        ctx->buflen = len;
    }
}

void md5_final(md5_ctx_t *ctx, uint8_t digest[MD5_DIGEST_SIZE])
{
    uint8_t pad[72];
    uint32_t padlen;
    uint64_t bitlen;
    uint32_t i;
    uint32_t w;

    /* 填充：0x80 + 0x00... + 64bit 位长（小端） */
    bitlen = ctx->bitlen;
    pad[0] = 0x80;
    padlen = (ctx->buflen < 56U) ? (56U - ctx->buflen) : (120U - ctx->buflen);
    memset(pad + 1, 0, padlen - 1U);

    /* 追加长度 */
    for (i = 0; i < 8; i++)
    {
        pad[padlen + i] = (uint8_t)((bitlen >> (i * 8U)) & 0xFFU);
    }

    md5_update(ctx, pad, padlen + 8U);

    /* 输出小端 */
    for (i = 0; i < 4; i++)
    {
        w = ctx->state[i];
        digest[i * 4]     = (uint8_t)(w & 0xFFU);
        digest[i * 4 + 1] = (uint8_t)((w >> 8) & 0xFFU);
        digest[i * 4 + 2] = (uint8_t)((w >> 16) & 0xFFU);
        digest[i * 4 + 3] = (uint8_t)((w >> 24) & 0xFFU);
    }
}

void md5_compute(const uint8_t *data, uint32_t len, uint8_t digest[MD5_DIGEST_SIZE])
{
    md5_ctx_t ctx;
    md5_init(&ctx);
    md5_update(&ctx, data, len);
    md5_final(&ctx, digest);
}

int md5_equal(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0;
    int i;
    for (i = 0; i < MD5_DIGEST_SIZE; i++)
    {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return (diff == 0) ? 1 : 0;
}

void md5_digest_to_hex(const uint8_t digest[MD5_DIGEST_SIZE], char *out)
{
    static const char hexc[] = "0123456789abcdef";
    int i;
    for (i = 0; i < MD5_DIGEST_SIZE; i++)
    {
        out[i * 2]     = hexc[digest[i] >> 4];
        out[i * 2 + 1] = hexc[digest[i] & 0x0F];
    }
    out[MD5_DIGEST_SIZE * 2] = '\0';
}
