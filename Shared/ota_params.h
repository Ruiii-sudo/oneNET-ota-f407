/**
 ******************************************************************************
 * @file    ota_params.h
 * @brief   参数区数据结构定义（BootLoader 与 App 工程共用）
 *
 * 参数区位于独立扇区 S3（0x0800C000），采用双槽冗余 + CRC32 校验：
 * 写参数时交替使用两个槽位，读取时校验 CRC，双槽均无效则回退默认值。
 ******************************************************************************
 */
#ifndef __OTA_PARAMS_H
#define __OTA_PARAMS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "partition.h"

/* 参数区有效标志 */
#define OTA_PARAM_MAGIC           0xA5A5A5A5UL

/* 参数区结构体：总长控制在 96 字节内（含 CRC），槽位 4KB 足够容纳多份冗余 */
typedef struct {
    uint32_t magic;         /* 有效标志 OTA_PARAM_MAGIC */
    uint32_t version;       /* 当前运行版本，如 V1.2.3 -> 0x010203 */
    uint32_t boot_flag;     /* OTA_BOOT_FLAG_NORMAL / OTA_BOOT_FLAG_TRY_NEW */
    uint32_t boot_count;    /* 启动计数：新固件连续启动失败次数 */
    uint32_t min_version;   /* 最低允许版本（防降级） */
    uint32_t resume_offset; /* 断点续传偏移（0 表示无未完成任务） */
    uint32_t active_slot;   /* 当前活动分区：0=App A，1=App B */
    uint32_t image_len;     /* 新固件镜像长度（字节，下载完成后写入） */
    uint8_t  backup_status; /* OTA_BACKUP_EMPTY / OTA_BACKUP_DOWNLOADING / OTA_BACKUP_VALID */
    uint8_t  image_sha[32]; /* 新固件 SHA-256 摘要（下载校验通过后写入） */
    uint32_t dl_tid;        /* 当前下载任务 ID（OneNET 任务号，续传/直接应用判定用） */
    uint32_t ota_tid;       /* 待上报 201（升级成功）的任务 ID，新固件运行确认后上报 */
    uint8_t  reserved[2];   /* 预留 */
    uint32_t crc32;         /* 结构体 CRC32（不含本字段） */
} ota_param_t;

#ifdef __cplusplus
}
#endif

#endif /* __OTA_PARAMS_H */
