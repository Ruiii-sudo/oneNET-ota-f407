/**
 ******************************************************************************
 * @file    boot_start.c
 * @brief   BootLoader 主流程（优化版：外部 W25Q16 暂存/备份/恢复）
 *
 * 运行模型：
 *   App 镜像固定链接/运行在槽位 A（0x08010000，448KB）；
 *   外部 W25Q16 分三个区：暂存区/备份区/恢复区。
 *
 * 升级流程：
 *   App 下载新固件到外部暂存区 -> 校验 SHA-256
 *   -> 备份旧 A 槽到外部备份区 -> 置 TRY_NEW 重启
 *   -> BootLoader 校验暂存区 -> 拷贝暂存区到 A -> 启动 A
 *
 * 回滚流程：
 *   新固件连续启动失败超过阈值 -> BootLoader 从备份区拷贝到 A
 *
 * 恢复流程：
 *   A 槽完全损坏 -> BootLoader 从恢复区拷贝到 A
 ******************************************************************************
 */
#include "boot_start.h"
#include "partition.h"
#include "ota_params.h"
#include "param_area.h"
#include "app_jump.h"
#include "flash_if.h"
#include "sha256.h"
#include "w25q16.h"
#include "iwdg.h"
#include "usart.h"

/* private functions */
static int  boot_verify_staging(const ota_param_t *param);
static int  boot_copy_staging_to_run(const ota_param_t *param);
static int  boot_rollback_from_backup(const ota_param_t *param);
static int  boot_recover_from_recovery(const ota_param_t *param);

/**
 * @brief  BootLoader main business
 */
void boot_run(void)
{
    ota_param_t param;
    uint32_t app_addr;

    USART1_Printf("\r\n==== STM32F407 OTA Bootloader v2.0 (External Flash) ====\r\n");

    /* ---- 0. init external flash ---- */
    w25q16_init();
    uint32_t flash_id = w25q16_read_jedec_id();
    USART1_Printf("[BL] ext flash ID: 0x%06lX\r\n", flash_id);

    /* ---- 1. load parameter ---- */
    if (param_area_load(&param) != 0)
    {
        USART1_Printf("[BL] param invalid, rebuild default\r\n");
        param_area_defaults(&param);
        if (param_area_save(&param) != 0)
        {
            USART1_Printf("[BL] param save FAILED!\r\n");
        }
    }
    USART1_Printf("[BL] flag=%lu count=%lu ver=0x%06lX len=%lu\r\n",
                  param.boot_flag, param.boot_count, param.version, param.image_len);

    /* ---- 2. ROLLBACK: copy backup to run slot ---- */
    if (param.boot_flag == OTA_BOOT_FLAG_ROLLBACK)
    {
        USART1_Printf("[BL] rollback requested, copy backup->A\r\n");
        if (boot_rollback_from_backup(&param) == 0)
        {
            USART1_Printf("[BL] rollback done\r\n");
			param.version = param.backup_version;   /* 回滚成功后，版本号同步为备份版本 */
        }
        else
        {
            USART1_Printf("[BL] rollback FAILED, try recovery\r\n");
            boot_recover_from_recovery(&param);
        }
        param.boot_flag = OTA_BOOT_FLAG_NORMAL;
        param.boot_count = 0;
        param.backup_status = OTA_BACKUP_EMPTY;
        param.backup_len = 0;
        param.backup_version = 0;
        if (param_area_save(&param) != 0)
        {
            USART1_Printf("[BL] param save FAILED!\r\n");
        }
    }
    /* ---- 2.5 RECOVERY: copy recovery (factory) to run slot ---- */
    else if (param.boot_flag == OTA_BOOT_FLAG_RECOVERY)
    {
        USART1_Printf("[BL] recovery requested, copy factory->A\r\n");
        if (boot_recover_from_recovery(&param) == 0)
        {
            USART1_Printf("[BL] recovery done\r\n");
        }
        else
        {
            USART1_Printf("[BL] recovery FAILED\r\n");
        }
        param.boot_flag = OTA_BOOT_FLAG_NORMAL;
        param.boot_count = 0;
        param.backup_status = OTA_BACKUP_EMPTY;
        param.backup_len = 0;
        param.backup_version = 0;
        if (param_area_save(&param) != 0)
        {
            USART1_Printf("[BL] param save FAILED!\r\n");
        }
    }
    /* ---- 3. TRY_NEW: verify staging, copy to run slot ---- */
    else if (param.boot_flag == OTA_BOOT_FLAG_TRY_NEW)
    {
        if (boot_verify_staging(&param) == 0)
        {
            USART1_Printf("[BL] staging verified, copy staging->A\r\n");
            if (boot_copy_staging_to_run(&param) == 0)
            {
                param.boot_count++;
                if (param.boot_count >= OTA_BOOT_MAX_TRIES)
                {
                    USART1_Printf("[BL] boot count=%lu >= %lu, rollback to backup\r\n",
                                  param.boot_count, OTA_BOOT_MAX_TRIES);
                    /* 启动失败太多次，回滚到备份版本 */
                    param.boot_flag = OTA_BOOT_FLAG_ROLLBACK;
                    param.boot_count = 0;
                }
                if (param_area_save(&param) != 0)
                {
                    USART1_Printf("[BL] param save FAILED!\r\n");
                }
            }
            else
            {
                /* copy interrupted (power loss etc.), retry next boot */
                USART1_Printf("[BL] copy FAILED, retry next boot\r\n");
                param.boot_count++;
                if (param.boot_count >= OTA_BOOT_MAX_TRIES)
                {
                    param.boot_flag = OTA_BOOT_FLAG_ROLLBACK;
                    param.boot_count = 0;
                }
                if (param_area_save(&param) != 0)
                {
                    USART1_Printf("[BL] param save FAILED!\r\n");
                }
            }
        }
        else
        {
            /* staging image invalid: try rollback */
            USART1_Printf("[BL] staging invalid, rollback to backup\r\n");
            param.boot_flag = OTA_BOOT_FLAG_ROLLBACK;
            param.boot_count = 0;
            if (param_area_save(&param) != 0)
            {
                USART1_Printf("[BL] param save FAILED!\r\n");
            }
        }
    }
    else
    {
        if (param.boot_count != 0)
        {
            param.boot_count = 0;
            if (param_area_save(&param) != 0)
            {
                USART1_Printf("[BL] param save FAILED!\r\n");
            }
        }
    }

    /* ---- 4. validate app A, if invalid try recovery ---- */
    app_addr = OTA_APP_A_ADDR;
    USART1_Printf("[BL] target app @0x%08lX\r\n", app_addr);

    if (app_jump_validate(app_addr) != 0)
    {
        USART1_Printf("[BL] app A invalid, try recovery!\r\n");
        boot_recover_from_recovery(&param);
    }

    /* ---- 5. jump ---- */
    USART1_Printf("[BL] jump to app @0x%08lX\r\n", app_addr);
    HAL_Delay(10);
    app_jump_execute(app_addr);
}

/**
 * @brief  verify staging image (external flash) SHA-256
 */
static int boot_verify_staging(const ota_param_t *param)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint32_t len = param->image_len;
    uint32_t remaining;
    uint8_t chunk[64];
    sha256_ctx_t ctx;

    if (len == 0 || len > OTA_APP_IMAGE_MAX_SIZE)
    {
        return -1;
    }

    sha256_init(&ctx);
    remaining = len;
    while (remaining > 0)
    {
        uint32_t take = (remaining > sizeof(chunk)) ? sizeof(chunk) : remaining;
        w25q16_read(EXT_STAGING_ADDR + (len - remaining), chunk, take);
        sha256_update(&ctx, chunk, take);
        remaining -= take;
        HAL_IWDG_Refresh(&hiwdg);
    }
    sha256_final(&ctx, digest);

    if (sha256_equal(digest, param->image_sha) != 1)
    {
        USART1_Printf("[BL] staging SHA-256 FAILED len=%lu\r\n", len);
        return -1;
    }
    USART1_Printf("[BL] staging SHA-256 OK\r\n");
    return 0;
}

/**
 * @brief  copy staging image (external flash) to run slot (internal A)
 */
static int boot_copy_staging_to_run(const ota_param_t *param)
{
    uint32_t dst = OTA_APP_A_ADDR;
    uint32_t len = param->image_len;
    uint32_t remain = len;
    uint8_t buf[1024];

    if (len == 0 || len > OTA_APP_IMAGE_MAX_SIZE)
    {
        return -1;
    }

    /* erase run-slot sectors one by one */
    {
        uint32_t cur = dst;
        uint32_t end = dst + len;
        while (cur < end)
        {
            if (flash_if_erase_addr(cur) != HAL_OK)
            {
                USART1_Printf("[BL] erase FAILED @0x%08lX\r\n", cur);
                return -1;
            }
            cur += flash_if_sector_size(cur);
            HAL_IWDG_Refresh(&hiwdg);
        }
    }

    while (remain > 0)
    {
        uint32_t take = (remain > sizeof(buf)) ? sizeof(buf) : remain;
        w25q16_read(EXT_STAGING_ADDR + (len - remain), buf, take);
        if (flash_if_write(dst + (len - remain), buf, take) != HAL_OK)
        {
            USART1_Printf("[BL] program run slot FAILED @0x%08lX\r\n",
                          dst + (len - remain));
            return -1;
        }
        remain -= take;
        HAL_IWDG_Refresh(&hiwdg);
    }

    USART1_Printf("[BL] copy staging->A done (%lu bytes)\r\n", len);
    return 0;
}

/**
 * @brief  rollback: copy backup (external flash) to run slot (internal A)
 */
static int boot_rollback_from_backup(const ota_param_t *param)
{
    uint32_t dst = OTA_APP_A_ADDR;
    uint32_t len = param->backup_len;
    uint32_t remain = len;
    uint8_t buf[1024];

    if (param->backup_status != OTA_BACKUP_VALID || len == 0 || len > OTA_APP_IMAGE_MAX_SIZE)
    {
        USART1_Printf("[BL] no valid backup\r\n");
        return -1;
    }

    /* erase run-slot sectors */
    {
        uint32_t cur = dst;
        uint32_t end = dst + len;
        while (cur < end)
        {
            if (flash_if_erase_addr(cur) != HAL_OK)
            {
                return -1;
            }
            cur += flash_if_sector_size(cur);
            HAL_IWDG_Refresh(&hiwdg);
        }
    }

    while (remain > 0)
    {
        uint32_t take = (remain > sizeof(buf)) ? sizeof(buf) : remain;
        w25q16_read(EXT_BACKUP_ADDR + (len - remain), buf, take);
        if (flash_if_write(dst + (len - remain), buf, take) != HAL_OK)
        {
            return -1;
        }
        remain -= take;
        HAL_IWDG_Refresh(&hiwdg);
    }

    USART1_Printf("[BL] rollback done, ver=0x%06lX\r\n", param->backup_version);
    return 0;
}

/**
 * @brief  recovery: copy recovery (external flash) to run slot (internal A)
 */
static int boot_recover_from_recovery(const ota_param_t *param)
{
    uint32_t dst = OTA_APP_A_ADDR;
    uint32_t len = param->recovery_len;
    uint32_t remain = len;
    uint8_t buf[1024];

    if (len == 0 || len > OTA_APP_IMAGE_MAX_SIZE)
    {
        USART1_Printf("[BL] recovery image len invalid\r\n");
        return -1;
    }

    /* erase run-slot sectors */
    {
        uint32_t cur = dst;
        uint32_t end = dst + len;
        while (cur < end)
        {
            if (flash_if_erase_addr(cur) != HAL_OK)
            {
                return -1;
            }
            cur += flash_if_sector_size(cur);
            HAL_IWDG_Refresh(&hiwdg);
        }
    }

    while (remain > 0)
    {
        uint32_t take = (remain > sizeof(buf)) ? sizeof(buf) : remain;
        w25q16_read(EXT_RECOVERY_ADDR + (len - remain), buf, take);
        if (flash_if_write(dst + (len - remain), buf, take) != HAL_OK)
        {
            return -1;
        }
        remain -= take;
        HAL_IWDG_Refresh(&hiwdg);
    }

    USART1_Printf("[BL] recovery done (%lu bytes)\r\n", len);
    return 0;
}
