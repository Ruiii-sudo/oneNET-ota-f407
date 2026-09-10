/**
 ******************************************************************************
 * @file    md5.h
 * @brief   MD5 摘要（RFC 1321），用于 OneNET 平台升级包完整性校验
 ******************************************************************************
 */
#ifndef __MD5_H
#define __MD5_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define MD5_DIGEST_SIZE 16

typedef struct {
    uint32_t state[4];
    uint64_t bitlen;
    uint8_t  buffer[64];
    uint32_t buflen;
} md5_ctx_t;

void md5_init(md5_ctx_t *ctx);
void md5_update(md5_ctx_t *ctx, const uint8_t *data, uint32_t len);
void md5_final(md5_ctx_t *ctx, uint8_t digest[MD5_DIGEST_SIZE]);

/* 便捷接口：一次计算完整数据的摘要 */
void md5_compute(const uint8_t *data, uint32_t len, uint8_t digest[MD5_DIGEST_SIZE]);

/* 便捷接口：比较两个摘要（常数时间比较） */
int  md5_equal(const uint8_t *a, const uint8_t *b);

/* 便捷接口：将摘要转换为小写十六进制字符串（out 需 >= 33 字节） */
void md5_digest_to_hex(const uint8_t digest[MD5_DIGEST_SIZE], char *out);

#ifdef __cplusplus
}
#endif

#endif /* __MD5_H */
