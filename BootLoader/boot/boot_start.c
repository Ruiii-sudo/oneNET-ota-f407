/**
 ******************************************************************************
 * @file    boot_start.c
 * @brief   BootLoader 主流程（OneNET 单升级包适配版）
 *
 * 运行模型（与 OneNET 平台单升级包兼容）：
 *   App 镜像固定链接/运行在槽位 A（0x08010000，project.sct）；
 *   槽位 B（0x08040000）仅作为升级暂存区。
 *   升级流程：App 下载新固件到 B -> 校验 SHA-256 -> 置 TRY_NEW 重启
 *             -> BootLoader 校验 B -> 拷贝 B 到 A -> 启动 A
 *   B 在校验/拷贝失败时始终保留新镜像，断电后下次启动可重试；
 *   新固件连续启动失败超过阈值则放弃 TRY_NEW（恢复靠平台重推旧版本）。
 ******************************************************************************
 */
#include "boot_start.h"
#include "partition.h"
#include "ota_params.h"
#include "param_area.h"
#include "app_jump.h"
#include "flash_if.h"
#include "sha256.h"
#include "iwdg.h"
#include "usart.h"

/* private functions */
static int  boot_verify_staging(const ota_param_t *param);
static int  boot_copy_staging_to_run(const ota_param_t *param);

/**
 * @brief  BootLoader main business
 */
void boot_run(void)
{
    ota_param_t param;
    uint32_t app_addr;

    USART1_Printf("\r\n==== STM32F407 OTA Bootloader v1.1 (OneNET) ====\r\n");

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

    /* ---- 2. TRY_NEW: verify staging(B), copy to run slot(A) ---- */
    if (param.boot_flag == OTA_BOOT_FLAG_TRY_NEW)
    {
        if (boot_verify_staging(&param) == 0)
        {
            USART1_Printf("[BL] staging verified, copy B->A\r\n");
            if (boot_copy_staging_to_run(&param) == 0)
            {
                param.boot_count++;
                if (param.boot_count >= OTA_BOOT_MAX_TRIES)
                {
                    USART1_Printf("[BL] boot count=%lu >= %lu, give up TRY_NEW\r\n",
                                  param.boot_count, OTA_BOOT_MAX_TRIES);
                    param.boot_flag = OTA_BOOT_FLAG_NORMAL;
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
                    param.boot_flag = OTA_BOOT_FLAG_NORMAL;
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
            /* staging image invalid/stale flag: clear and boot A as-is */
            USART1_Printf("[BL] staging invalid, give up TRY_NEW\r\n");
            param.boot_flag = OTA_BOOT_FLAG_NORMAL;
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

    /* ---- 3. always run from slot A (fixed link address) ---- */
    app_addr = OTA_APP_A_ADDR;
    USART1_Printf("[BL] target app @0x%08lX\r\n", app_addr);

    if (app_jump_validate(app_addr) != 0)
    {
        USART1_Printf("[BL] app invalid, stay in bootloader!\r\n");
        while (1)
        {
            HAL_IWDG_Refresh(&hiwdg);   /* keep watchdog alive */
            HAL_Delay(100);
        }
    }

    /* ---- 4. jump ---- */
    USART1_Printf("[BL] jump to app @0x%08lX\r\n", app_addr);
    HAL_Delay(10);
    app_jump_execute(app_addr);
}

/**
 * @brief  verify staging image (slot B) SHA-256 against parameter record
 */
static int boot_verify_staging(const ota_param_t *param)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint32_t len = param->image_len;
    uint32_t remaining;
    uint8_t chunk[64];
    sha256_ctx_t ctx;

    if (param->backup_status != OTA_BACKUP_VALID || len == 0 || len > OTA_APP_IMAGE_MAX_SIZE)
    {
        return -1;
    }

    sha256_init(&ctx);
    remaining = len;
    while (remaining > 0)
    {
        uint32_t take = (remaining > sizeof(chunk)) ? sizeof(chunk) : remaining;
        flash_if_read(OTA_APP_B_ADDR + (len - remaining), chunk, take);
        sha256_update(&ctx, chunk, take);
        remaining -= take;
        HAL_IWDG_Refresh(&hiwdg);   /* keep watchdog alive while verifying */
    }
    sha256_final(&ctx, digest);

    if (sha256_equal(digest, param->image_sha) != 1)
    {
        USART1_Printf("[BL] staging SHA-256 FAILED @0x%08lX len=%lu\r\n", OTA_APP_B_ADDR, len);
        return -1;
    }
    USART1_Printf("[BL] staging SHA-256 OK\r\n");
    return 0;
}

/**
 * @brief  copy staging image (slot B) to run slot (slot A)
 *         B keeps intact on failure, so next boot can retry.
 */
static int boot_copy_staging_to_run(const ota_param_t *param)
{
    uint32_t src = OTA_APP_B_ADDR;
    uint32_t dst = OTA_APP_A_ADDR;
    uint32_t len = param->image_len;
    uint32_t remain = len;
    uint8_t buf[1024];

    if (len == 0 || len > OTA_APP_IMAGE_MAX_SIZE)
    {
        return -1;
    }

    /* erase run-slot sectors (S4-S5) one by one, refresh watchdog between
       each; bootloader itself stays in S0-S2, so this is safe */
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
        flash_if_read(src + (len - remain), buf, take);
        if (flash_if_write(dst + (len - remain), buf, take) != HAL_OK)
        {
            USART1_Printf("[BL] program run slot FAILED @0x%08lX\r\n",
                          dst + (len - remain));
            return -1;
        }
        remain -= take;
        HAL_IWDG_Refresh(&hiwdg);   /* keep watchdog alive while copying */
    }

    USART1_Printf("[BL] copy B->A done (%lu bytes)\r\n", len);
    return 0;
}
