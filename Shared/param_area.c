/**
 ******************************************************************************
 * @file    param_area.c
 * @brief   参数区管理
 *
 * 存储策略：
 *  - 两个 4KB 槽位（SLOT_A / SLOT_B），同处参数区扇区（S3，16KB）。
 *  - 保存时擦除整个参数区扇区，再依次写 A、B 两槽。
 *  - 读取时先校验 A 槽（magic + CRC），无效则校验 B 槽。
 *  - 擦除参数区扇区只影响参数本身，不影响 App 分区（独立扇区设计）。
 ******************************************************************************
 */
#include "param_area.h"
#include "flash_if.h"
#include "crc32.h"
#include <string.h>

uint32_t param_area_crc(const ota_param_t *param)
{
    /* 不含 crc32 字段（最后一个 4 字节） */
    return crc32_calc((const uint8_t *)param, sizeof(ota_param_t) - 4U);
}

void param_area_defaults(ota_param_t *param)
{
    memset(param, 0, sizeof(ota_param_t));
    param->magic         = OTA_PARAM_MAGIC;
    param->version       = OTA_VERSION(1, 0, 0);
    param->boot_flag     = OTA_BOOT_FLAG_NORMAL;
    param->boot_count    = 0;
    param->min_version   = OTA_VERSION(1, 0, 0);
    param->resume_offset = 0;
    param->active_slot   = 0;   /* 默认 App A */
    param->backup_status = OTA_BACKUP_EMPTY;
    param->crc32         = param_area_crc(param);
}

/* 读取单个槽位并校验，成功返回 0 */
static int slot_load(uint32_t slot_addr, ota_param_t *out)
{
    ota_param_t tmp;
    uint32_t crc;

    flash_if_read(slot_addr, (uint8_t *)&tmp, sizeof(tmp));
    if (tmp.magic != OTA_PARAM_MAGIC)
    {
        return -1;
    }
    crc = param_area_crc(&tmp);
    if (crc != tmp.crc32)
    {
        return -1;
    }
    memcpy(out, &tmp, sizeof(ota_param_t));
    return 0;
}

int param_area_load(ota_param_t *param)
{
    if (slot_load(OTA_PARAM_SLOT_A_ADDR, param) == 0)
    {
        return 0;
    }
    if (slot_load(OTA_PARAM_SLOT_B_ADDR, param) == 0)
    {
        return 0;
    }
    return -1;
}

int param_area_save(const ota_param_t *param)
{
    ota_param_t tmp;
    uint32_t crc;

    memcpy(&tmp, param, sizeof(ota_param_t));
    crc = param_area_crc(&tmp);
    tmp.crc32 = crc;

    /* 擦除参数区扇区（S3，16KB，独立扇区） */
    if (flash_if_erase_addr(OTA_PARAM_AREA_ADDR) != HAL_OK)
    {
        return -1;
    }

    /* 写槽位 A */
    if (flash_if_write(OTA_PARAM_SLOT_A_ADDR, (const uint8_t *)&tmp, sizeof(tmp)) != HAL_OK)
    {
        return -1;
    }

    /* 写槽位 B（冗余） */
    if (flash_if_write(OTA_PARAM_SLOT_B_ADDR, (const uint8_t *)&tmp, sizeof(tmp)) != HAL_OK)
    {
        return -1;
    }

    return 0;
}
