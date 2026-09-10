/**
 ******************************************************************************
 * @file    crc32.h
 * @brief   CRC-32 (IEEE 802.3) 校验
 ******************************************************************************
 */
#ifndef __CRC32_H
#define __CRC32_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* 计算 CRC-32，poly=0xEDB88320，初值 0xFFFFFFFF，结果异或 0xFFFFFFFF */
uint32_t crc32_calc(const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* __CRC32_H */
