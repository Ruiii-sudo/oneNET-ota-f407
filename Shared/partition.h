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
 * 片内分区布局（优化版：单 A 槽 + 外部 Flash 暂存/备份/恢复）:
 *   Bootloader  48KB @ 0x08000000  (S0-S2)
 *   参数区      16KB @ 0x0800C000  (S3)
 *   App A(Active) 448KB @ 0x08010000 (S4-S7)
 *
 * 外部 W25Q16 (2MB) 分区布局:
 *   暂存区(Staging)  448KB @ 0x00000000  (新固件下载到这里)
 *   备份区(Backup)   448KB @ 0x00070000  (旧版本备份)
 *   恢复区(Recovery) 448KB @ 0x000E0000  (出厂固件)
 *   资源区(Resource) ~704KB @ 0x00150000
 ******************************************************************************
 */
#ifndef __PARTITION_H
#define __PARTITION_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ========== 片内 Flash 分区 ========== */

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

/* App A 分区（S4-S7，448KB） */
#define OTA_APP_A_ADDR            0x08010000UL
#define OTA_APP_SIZE              0x00070000UL   /* 448KB */

/* 固件镜像上限：448KB（与 App A 分区 S4-S7 完全一致） */
#define OTA_APP_IMAGE_MAX_SIZE    0x00070000UL

/* ========== 外部 W25Q16 分区 ========== */

/* 外部 Flash 总容量 2MB */
#define EXT_FLASH_SIZE            (2UL * 1024UL * 1024UL)

/* 外部 Flash 分区大小（各 448KB） */
#define EXT_REGION_SIZE           0x00070000UL   /* 448KB */

/* 暂存区：新固件下载到这里 */
#define EXT_STAGING_ADDR          0x00000000UL

/* 备份区：旧版本备份 */
#define EXT_BACKUP_ADDR           (EXT_STAGING_ADDR + EXT_REGION_SIZE)   /* 0x00070000 */

/* 恢复区：出厂固件 */
#define EXT_RECOVERY_ADDR         (EXT_BACKUP_ADDR + EXT_REGION_SIZE)    /* 0x000E0000 */

/* 资源区：剩余空间 */
#define EXT_RESOURCE_ADDR         (EXT_RECOVERY_ADDR + EXT_REGION_SIZE)  /* 0x00150000 */

/* ========== 启动标志 ========== */

#define OTA_BOOT_FLAG_NORMAL      0UL            /* 正常启动 */
#define OTA_BOOT_FLAG_TRY_NEW     1UL            /* 尝试启动新固件 */
#define OTA_BOOT_FLAG_ROLLBACK    2UL            /* 回滚到备份版本 */
#define OTA_BOOT_FLAG_RECOVERY    3UL            /* 恢复出厂设置（从恢复区拷贝） */

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
