/**
 ******************************************************************************
 * @file    sha256.h
 * @brief   SHA-256 摘要（用于固件完整性校验，无依赖）
 ******************************************************************************
 */
#ifndef __SHA256_H
#define __SHA256_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  buffer[64];
    uint32_t buflen;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, uint32_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_SIZE]);

/* 便捷接口：一次计算完整数据的摘要 */
void sha256_compute(const uint8_t *data, uint32_t len, uint8_t digest[SHA256_DIGEST_SIZE]);

/* 便捷接口：比较两个摘要（常数时间比较） */
int  sha256_equal(const uint8_t *a, const uint8_t *b);

#ifdef __cplusplus
}
#endif

#endif /* __SHA256_H */
