/**
 ******************************************************************************
 * @file    crc32.c
 * @brief   CRC-32 (IEEE 802.3) 校验，位运算实现（无查表，代码紧凑）
 ******************************************************************************
 */
#include "crc32.h"

uint32_t crc32_calc(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t i, j;

    for (i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (j = 0; j < 8; j++)
        {
            if (crc & 1UL)
            {
                crc = (crc >> 1) ^ 0xEDB88320UL;
            }
            else
            {
                crc >>= 1;
            }
        }
    }

    return crc ^ 0xFFFFFFFFUL;
}
