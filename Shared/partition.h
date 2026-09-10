/**
 ******************************************************************************
 * @file    partition.h
 * @brief   OTA 分区布局定义（BootLoader 与 App 工程共用同一份约定）
 *
 * STM32F407VET6: 512KB Flash, 扇区布局:
 *   S0 16KB @0x08000000 | S1 16KB @0x08004000 | S2 16KB @0x08008000
 *   S3 16KB @0x0800C000 | S4 64KB @0x08010000 | S5 128KB @0x08020000
 *   S6 128KB @0x08040000 | S7 128KB @0x08060000
 *
 * 分区布局（参数区独立扇区，避免跨扇区擦除冲突）:
 *   Bootloader  48KB @ 0x08000000  (S0-S2)
 *   参数区      16KB @ 0x0800C000  (S3)
 *   App A(Active) 192KB @ 0x08010000 (S4-S5)
 *   App B(Backup) 256KB @ 0x08040000 (S6-S7)
 ******************************************************************************
 */
#ifndef __PARTITION_H
#define __PARTITION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Flash 总容量 */
#define OTA_FLASH_SIZE            (512UL * 1024UL)

/* Bootloader 分区（S0-S2，共 48KB） */
#define OTA_BOOTLOADER_ADDR       0x08000000UL
#define OTA_BOOTLOADER_SIZE       0x0000C000UL   /* 48KB */

/* 参数区（S3，16KB，独立扇区） */
#define OTA_PARAM_AREA_ADDR       0x0800C000UL
#define OTA_PARAM_AREA_SIZE       0x00004000UL   /* 16KB */

/* 参数区槽位（两个 4KB 槽位冗余） */
#define OTA_PARAM_SLOT_SIZE       0x00001000UL
#define OTA_PARAM_SLOT_A_ADDR     (OTA_PARAM_AREA_ADDR)
#define OTA_PARAM_SLOT_B_ADDR     (OTA_PARAM_AREA_ADDR + OTA_PARAM_SLOT_SIZE)

/* App 分区 */
#define OTA_APP_A_ADDR            0x08010000UL   /* Active 分区（S4-S5，192KB） */
#define OTA_APP_B_ADDR            0x08040000UL   /* Backup 分区（S6-S7，256KB） */
#define OTA_APP_SIZE              0x00030000UL   /* App A 分区大小 192KB */

/* 固件镜像上限：192KB（与 App A 分区 S4-S5 完全一致，当前固件 196608B） */
#define OTA_APP_IMAGE_MAX_SIZE    0x00030000UL

/* 启动标志 */
#define OTA_BOOT_FLAG_NORMAL      0UL            /* 正常启动 */
#define OTA_BOOT_FLAG_TRY_NEW     1UL            /* 尝试启动新固件 */

/* Backup 分区状态 */
#define OTA_BACKUP_EMPTY          0x00U
#define OTA_BACKUP_DOWNLOADING    0x01U
#define OTA_BACKUP_VALID          0x02U

/* 启动计数阈值：连续失败超过该次数则自动回滚 */
#define OTA_BOOT_MAX_TRIES        3UL

/* 版本号编码宏：V1.2.3 -> 0x010203 */
#define OTA_VERSION(major, minor, patch) \
    ((((uint32_t)(major)) << 16) | (((uint32_t)(minor)) << 8) | ((uint32_t)(patch)))

#ifdef __cplusplus
}
#endif

#endif /* __PARTITION_H */
