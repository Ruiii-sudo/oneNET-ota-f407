/**
 ******************************************************************************
 * @file    flash_if.c
 * @brief   内部 Flash 读写擦驱动（STM32F407VET6：16KB*4 + 64KB + 128KB*3）
 ******************************************************************************
 */
#include "flash_if.h"
#include <string.h>

/* 扇区起始地址表（F407 共 12 个扇区） */
static const uint32_t s_sector_start[12] = {
    0x08000000UL, 0x08004000UL, 0x08008000UL, 0x0800C000UL, /* S0-S3 16KB */
    0x08010000UL,                                            /* S4 64KB */
    0x08020000UL, 0x08040000UL, 0x08060000UL,                /* S5-S7 128KB */
    0x08080000UL, 0x080A0000UL, 0x080C0000UL                 /* S8-S11（1MB 型号） */
};

/* 扇区大小表 */
static const uint32_t s_sector_size[12] = {
    16UL * 1024UL, 16UL * 1024UL, 16UL * 1024UL, 16UL * 1024UL,
    64UL * 1024UL,
    128UL * 1024UL, 128UL * 1024UL, 128UL * 1024UL,
    128UL * 1024UL, 128UL * 1024UL, 128UL * 1024UL, 128UL * 1024UL
};

uint32_t flash_if_sector_start(uint32_t addr)
{
    return s_sector_start[flash_if_sector_number(addr)];
}

uint32_t flash_if_sector_size(uint32_t addr)
{
    return s_sector_size[flash_if_sector_number(addr)];
}

uint32_t flash_if_sector_number(uint32_t addr)
{
    uint32_t i;
    for (i = 0; i < 12; i++)
    {
        if (addr < s_sector_start[i] + s_sector_size[i])
        {
            return i;
        }
    }
    return 11;
}

HAL_StatusTypeDef flash_if_erase_addr(uint32_t addr)
{
    HAL_StatusTypeDef st;
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t sector = flash_if_sector_number(addr);
    uint32_t error = 0;

    st = HAL_FLASH_Unlock();
    if (st != HAL_OK) return st;

    erase.TypeErase   = FLASH_TYPEERASE_SECTORS;
    erase.Sector      = sector;
    erase.NbSectors   = 1;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASHEx_Erase(&erase, &error) != HAL_OK)
    {
        HAL_FLASH_Lock();
        return HAL_ERROR;
    }

    HAL_FLASH_Lock();
    return HAL_OK;
}

HAL_StatusTypeDef flash_if_erase_region(uint32_t base, uint32_t size)
{
    HAL_StatusTypeDef st;
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t first, last, nsectors, error = 0;

    if (size == 0) return HAL_OK;

    first = flash_if_sector_number(base);
    last  = flash_if_sector_number(base + size - 1);
    nsectors = last - first + 1;

    st = HAL_FLASH_Unlock();
    if (st != HAL_OK) return st;

    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.Sector       = first;
    erase.NbSectors    = nsectors;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    if (HAL_FLASHEx_Erase(&erase, &error) != HAL_OK)
    {
        HAL_FLASH_Lock();
        return HAL_ERROR;
    }

    HAL_FLASH_Lock();
    return HAL_OK;
}

HAL_StatusTypeDef flash_if_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    HAL_StatusTypeDef st;
    uint32_t i = 0;

    if (len == 0) return HAL_OK;
    if ((addr & 3U) != 0U) return HAL_ERROR;   /* 32 位字编程必须 4 字节对齐 */

    st = HAL_FLASH_Unlock();
    if (st != HAL_OK) return st;

    /* 按 32 位字写入主体 */
    for (; i + 4U <= len; i += 4U)
    {
        uint32_t w;
        memcpy(&w, data + i, 4);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, w) != HAL_OK)
        {
            HAL_FLASH_Lock();
            return HAL_ERROR;
        }
    }

    /* 尾部不足 4 字节：按字补齐 0xFF 后写入 */
    if (i < len)
    {
        uint32_t w = 0xFFFFFFFFUL;
        memcpy(&w, data + i, len - i);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, w) != HAL_OK)
        {
            HAL_FLASH_Lock();
            return HAL_ERROR;
        }
    }

    HAL_FLASH_Lock();

    /* 回读校验：逐字节比对 */
    {
        uint8_t check;
        uint32_t j;
        for (j = 0; j < len; j++)
        {
            check = *((const uint8_t *)(addr + j));
            if (check != data[j])
            {
                return HAL_ERROR;
            }
        }
    }

    return HAL_OK;
}

void flash_if_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    memcpy(buf, (const void *)addr, len);
}
