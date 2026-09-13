/**
 ******************************************************************************
 * @file    param_area.c
 * @brief   参数区管理（轮转写入 / 环形日志，掉电安全）
 *
 * 存储策略：
 *  - S3 扇区（16KB @ 0x0800C000）划分为 32 个 512B 槽位。
 *  - 每次保存写入下一个空槽（magic + seq + crc32 校验），不擦除扇区。
 *  - 32 个槽写满后才整扇区擦除一次，回到槽 0。
 *  - 读取时扫描全部槽，取 seq 最大且校验通过的有效槽。
 *  - 断电时旧数据始终保留，最多丢当前这一次写入（擦除窗口从"每次保存
 *    必现"降到"每 32 次保存一次"）。
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
    param->backup_status = OTA_BACKUP_EMPTY;
    param->backup_version = 0;
    param->backup_len    = 0;
    param->recovery_len  = 0;
    param->crc32         = param_area_crc(param);
}

/* ---------- 槽位内部格式：magic(4) + seq(4) + ota_param_t ---------- */

typedef struct {
    uint32_t magic;             /* 槽有效标志 OTA_PARAM_MAGIC */
    uint32_t seq;               /* 序号，单调递增（区分新旧） */
    ota_param_t param;          /* 参数结构体（含内部 crc32） */
} param_slot_t;

#define PARAM_SLOT_HEADER_SIZE  8U

/* 判断槽是否为空（头部 16 字节全 0xFF） */
static int slot_is_empty(uint32_t slot_addr)
{
    uint8_t buf[16];

    flash_if_read(slot_addr, buf, sizeof(buf));
    for (uint32_t i = 0; i < sizeof(buf); i++)
    {
        if (buf[i] != 0xFF)
        {
            return 0;
        }
    }
    return 1;
}

/* 读取单个槽位并校验，有效返回 0 并输出参数和序号 */
static int slot_load(uint32_t slot_addr, ota_param_t *out, uint32_t *out_seq)
{
    param_slot_t tmp;

    flash_if_read(slot_addr, (uint8_t *)&tmp, PARAM_SLOT_HEADER_SIZE + sizeof(ota_param_t));

    if (tmp.magic != OTA_PARAM_MAGIC)
    {
        return -1;
    }
    if (param_area_crc(&tmp.param) != tmp.param.crc32)
    {
        return -1;
    }
    if (out)     memcpy(out, &tmp.param, sizeof(ota_param_t));
    if (out_seq) *out_seq = tmp.seq;
    return 0;
}

int param_area_load(ota_param_t *param)
{
    int found = 0;
    uint32_t best_seq = 0;
    ota_param_t best;

    for (uint32_t i = 0; i < OTA_PARAM_SLOT_COUNT; i++)
    {
        uint32_t seq;
        ota_param_t tmp;
        uint32_t addr = OTA_PARAM_SLOT0_ADDR + i * OTA_PARAM_SLOT_SIZE;

        if (slot_load(addr, &tmp, &seq) == 0)
        {
            /* 取 seq 最大的槽（差值比较，处理 uint32 回绕） */
            if (!found || (int32_t)(seq - best_seq) > 0)
            {
                best_seq = seq;
                best     = tmp;
                found    = 1;
            }
        }
    }

    if (found)
    {
        memcpy(param, &best, sizeof(ota_param_t));
        return 0;
    }
    return -1;
}

int param_area_save(const ota_param_t *param)
{
    ota_param_t tmp;
    uint32_t cur_seq = 0;
    uint32_t target = 0;
    int has_empty = 0;
    param_slot_t slot;

    /* 1. 找到当前最大有效序号 */
    for (uint32_t i = 0; i < OTA_PARAM_SLOT_COUNT; i++)
    {
        uint32_t seq;
        uint32_t addr = OTA_PARAM_SLOT0_ADDR + i * OTA_PARAM_SLOT_SIZE;
        if (slot_load(addr, NULL, &seq) == 0)
        {
            if ((int32_t)(seq - cur_seq) > 0) cur_seq = seq;
        }
    }

    memcpy(&tmp, param, sizeof(ota_param_t));
    tmp.crc32 = param_area_crc(&tmp);

    /* 2. 找下一个空槽 */
    for (uint32_t i = 0; i < OTA_PARAM_SLOT_COUNT; i++)
    {
        uint32_t addr = OTA_PARAM_SLOT0_ADDR + i * OTA_PARAM_SLOT_SIZE;
        if (slot_is_empty(addr))
        {
            target = i;
            has_empty = 1;
            break;
        }
    }

    /* 3. 没有空槽：整扇区擦除，从头写 */
    if (!has_empty)
    {
        if (flash_if_erase_addr(OTA_PARAM_AREA_ADDR) != HAL_OK)
        {
            return -1;
        }
        target = 0;
    }

    /* 4. 写槽位（magic + seq + 参数） */
    slot.magic = OTA_PARAM_MAGIC;
    slot.seq   = cur_seq + 1U;    /* 序号继续递增 */
    memcpy(&slot.param, &tmp, sizeof(ota_param_t));

    if (flash_if_write(OTA_PARAM_SLOT0_ADDR + target * OTA_PARAM_SLOT_SIZE,
                       (const uint8_t *)&slot,
                       PARAM_SLOT_HEADER_SIZE + sizeof(ota_param_t)) != HAL_OK)
    {
        return -1;
    }

    return 0;
}
