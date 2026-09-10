/**
 ******************************************************************************
 * @file    flash_if.h
 * @brief   内部 Flash 读写擦驱动（基于 STM32 HAL，STM32F407）
 ******************************************************************************
 */
#ifndef __FLASH_IF_H
#define __FLASH_IF_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"

/* 返回地址所在扇区的起始地址 */
uint32_t flash_if_sector_start(uint32_t addr);

/* 返回地址所在扇区的大小（字节） */
uint32_t flash_if_sector_size(uint32_t addr);

/* 返回地址所在扇区号（FLASH_SECTOR_0 .. FLASH_SECTOR_11） */
uint32_t flash_if_sector_number(uint32_t addr);

/* 擦除 addr 所在扇区 */
HAL_StatusTypeDef flash_if_erase_addr(uint32_t addr);

/* 擦除 [base, base+size) 覆盖的全部扇区 */
HAL_StatusTypeDef flash_if_erase_region(uint32_t base, uint32_t size);

/*
 * 写入 len 字节到 addr（按 32 位字编程，自动处理尾部不足 4 字节）
 * 写入后回读校验，不一致返回 HAL_ERROR
 */
HAL_StatusTypeDef flash_if_write(uint32_t addr, const uint8_t *data, uint32_t len);

/* 读取 len 字节 */
void flash_if_read(uint32_t addr, uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* __FLASH_IF_H */
