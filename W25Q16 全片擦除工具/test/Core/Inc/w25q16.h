/**
 ******************************************************************************
 * @file    w25q16.h
 * @brief   W25Q16 SPI Flash 驱动（2MB）
 *
 * 引脚（原理图已确认）：
 *   CS  → PA15 (W25Q16_CS)
 *   SCK → PB3 (SPI3_SCK)
 *   MISO→ PB4 (SPI3_MISO)
 *   MOSI→ PB5 (SPI3_MOSI)
 ******************************************************************************
 */
#ifndef __W25Q16_H
#define __W25Q16_H

#include "main.h"
#include <stdint.h>

/* Flash 容量参数（W25Q16 = 2MB） */
#define W25Q16_FLASH_SIZE       (2 * 1024 * 1024)   /* 2MB */
#define W25Q16_SECTOR_SIZE      (4 * 1024)          /* 4KB 扇区 */
#define W25Q16_PAGE_SIZE        256                 /* 256 字节页 */
#define W25Q16_SECTOR_COUNT    (W25Q16_FLASH_SIZE / W25Q16_SECTOR_SIZE)

/* W25Q 命令（W25Q16/W25Q64 通用） */
#define W25Q16_CMD_WRITE_ENABLE     0x06
#define W25Q16_CMD_WRITE_DISABLE    0x04
#define W25Q16_CMD_READ_STATUS1    0x05
#define W25Q16_CMD_READ_STATUS2    0x35
#define W25Q16_CMD_PAGE_PROGRAM     0x02
#define W25Q16_CMD_SECTOR_ERASE     0x20    /* 4KB 扇区擦除 */
#define W25Q16_CMD_BLOCK_ERASE      0xD8   /* 64KB 块擦除 */
#define W25Q16_CMD_CHIP_ERASE       0xC7
#define W25Q16_CMD_READ_DATA        0x03
#define W25Q16_CMD_FAST_READ        0x0B
#define W25Q16_CMD_JEDEC_ID         0x9F

/* 状态寄存器位 */
#define W25Q16_STATUS_BUSY          0x01
#define W25Q16_STATUS_WEL          0x02

/* 函数原型 */
void    w25q16_init(void);
uint32_t w25q16_read_jedec_id(void);
uint8_t w25q16_read_status1(void);
uint8_t w25q16_read_status2(void);
void    w25q16_wait_busy(void);
void    w25q16_write_enable(void);
void    w25q16_write_disable(void);
HAL_StatusTypeDef w25q16_erase_sector(uint32_t addr);
HAL_StatusTypeDef w25q16_erase_block(uint32_t addr);
HAL_StatusTypeDef w25q16_erase_chip(void);
HAL_StatusTypeDef w25q16_write(uint32_t addr, const uint8_t *data, uint32_t len);
HAL_StatusTypeDef w25q16_read(uint32_t addr, uint8_t *data, uint32_t len);

#endif /* __W25Q16_H */
