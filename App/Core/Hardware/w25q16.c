/**
 ******************************************************************************
 * @file    w25q16.c
 * @brief   W25Q16 SPI Flash 驱动实现（2MB）
 ******************************************************************************
 */
#include "w25q16.h"
#include "spi.h"
#include "main.h"

/* CS 控制宏（和 CubeMX 里的 User Label 对应） */
#define W25Q16_CS_LOW()    HAL_GPIO_WritePin(W25Q16_CS_GPIO_Port, W25Q16_CS_Pin, GPIO_PIN_RESET)
#define W25Q16_CS_HIGH()   HAL_GPIO_WritePin(W25Q16_CS_GPIO_Port, W25Q16_CS_Pin, GPIO_PIN_SET)

extern SPI_HandleTypeDef hspi3;

/* SPI 收发一个字节 */
static uint8_t spi_xfer_byte(uint8_t tx)
{
    uint8_t rx;
    HAL_SPI_TransmitReceive(&hspi3, &tx, &rx, 1, HAL_MAX_DELAY);
    return rx;
}

void w25q16_init(void)
{
    /* CS 默认拉高 */
    W25Q16_CS_HIGH();
}

uint32_t w25q16_read_jedec_id(void)
{
    uint8_t id[3];
    W25Q16_CS_LOW();
    spi_xfer_byte(W25Q16_CMD_JEDEC_ID);
    id[0] = spi_xfer_byte(0xFF);  /* Manufacturer ID */
    id[1] = spi_xfer_byte(0xFF);  /* Memory Type */
    id[2] = spi_xfer_byte(0xFF);  /* Capacity */
    W25Q16_CS_HIGH();
    return ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
}

uint8_t w25q16_read_status1(void)
{
    uint8_t val;
    W25Q16_CS_LOW();
    spi_xfer_byte(W25Q16_CMD_READ_STATUS1);
    val = spi_xfer_byte(0xFF);
    W25Q16_CS_HIGH();
    return val;
}

uint8_t w25q16_read_status2(void)
{
    uint8_t val;
    W25Q16_CS_LOW();
    spi_xfer_byte(W25Q16_CMD_READ_STATUS2);
    val = spi_xfer_byte(0xFF);
    W25Q16_CS_HIGH();
    return val;
}

void w25q16_wait_busy(void)
{
    while (w25q16_read_status1() & W25Q16_STATUS_BUSY)
    {
        /* 等待 BUSY 清零 */
    }
}

void w25q16_write_enable(void)
{
    W25Q16_CS_LOW();
    spi_xfer_byte(W25Q16_CMD_WRITE_ENABLE);
    W25Q16_CS_HIGH();
}

void w25q16_write_disable(void)
{
    W25Q16_CS_LOW();
    spi_xfer_byte(W25Q16_CMD_WRITE_DISABLE);
    W25Q16_CS_HIGH();
}

HAL_StatusTypeDef w25q16_erase_sector(uint32_t addr)
{
    if (addr >= W25Q16_FLASH_SIZE) return HAL_ERROR;
    w25q16_write_enable();
    W25Q16_CS_LOW();
    spi_xfer_byte(W25Q16_CMD_SECTOR_ERASE);
    spi_xfer_byte((addr >> 16) & 0xFF);
    spi_xfer_byte((addr >> 8) & 0xFF);
    spi_xfer_byte(addr & 0xFF);
    W25Q16_CS_HIGH();
    w25q16_wait_busy();
    return HAL_OK;
}

HAL_StatusTypeDef w25q16_erase_block(uint32_t addr)
{
    if (addr >= W25Q16_FLASH_SIZE) return HAL_ERROR;
    w25q16_write_enable();
    W25Q16_CS_LOW();
    spi_xfer_byte(W25Q16_CMD_BLOCK_ERASE);
    spi_xfer_byte((addr >> 16) & 0xFF);
    spi_xfer_byte((addr >> 8) & 0xFF);
    spi_xfer_byte(addr & 0xFF);
    W25Q16_CS_HIGH();
    w25q16_wait_busy();
    return HAL_OK;
}

HAL_StatusTypeDef w25q16_erase_chip(void)
{
    w25q16_write_enable();
    W25Q16_CS_LOW();
    spi_xfer_byte(W25Q16_CMD_CHIP_ERASE);
    W25Q16_CS_HIGH();
    w25q16_wait_busy();
    return HAL_OK;
}

HAL_StatusTypeDef w25q16_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (addr + len > W25Q16_FLASH_SIZE) return HAL_ERROR;

    while (len > 0)
    {
        w25q16_write_enable();
        W25Q16_CS_LOW();
        spi_xfer_byte(W25Q16_CMD_PAGE_PROGRAM);
        spi_xfer_byte((addr >> 16) & 0xFF);
        spi_xfer_byte((addr >> 8) & 0xFF);
        spi_xfer_byte(addr & 0xFF);

        /* 计算当前页剩余空间 */
        uint32_t page_remain = W25Q16_PAGE_SIZE - (addr % W25Q16_PAGE_SIZE);
        uint32_t chunk = (len < page_remain) ? len : page_remain;

        for (uint32_t i = 0; i < chunk; i++)
        {
            spi_xfer_byte(data[i]);
        }
        W25Q16_CS_HIGH();
        w25q16_wait_busy();

        addr += chunk;
        data += chunk;
        len -= chunk;
    }
    return HAL_OK;
}

HAL_StatusTypeDef w25q16_read(uint32_t addr, uint8_t *data, uint32_t len)
{
    if (addr + len > W25Q16_FLASH_SIZE) return HAL_ERROR;

    W25Q16_CS_LOW();
    spi_xfer_byte(W25Q16_CMD_READ_DATA);
    spi_xfer_byte((addr >> 16) & 0xFF);
    spi_xfer_byte((addr >> 8) & 0xFF);
    spi_xfer_byte(addr & 0xFF);

    for (uint32_t i = 0; i < len; i++)
    {
        data[i] = spi_xfer_byte(0xFF);
    }
    W25Q16_CS_HIGH();
    return HAL_OK;
}
