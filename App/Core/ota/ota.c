/**
 ******************************************************************************
 * @file    ota.c
 * @brief   OTA 升级状态机与任务实现（App 侧）—— OneNET/CMIoT 平台内置远程升级
 *
 * 升级流程（平台"增值服务 -> OTA升级"）：
 *  1. 若处于 TRY_NEW（刚切换新固件），正常运行满窗口后固化成功标志；
 *     随后向平台上报 201（升级成功），平台将设备版本更新为目标版本
 *  2. 连接 WiFi -> HTTP 阶段（MQTT 未连接，避免 ESP 单连接冲突）：
 *     a) 若有待上报的 201 -> 上报并清除
 *     b) 上报当前固件版本（POST /fuse-ota/.../version）
 *     c) 检测升级任务（GET /fuse-ota/.../check）
 *  3. 有任务 -> 断点续传下载升级包（GET /fuse-ota/.../{tid}/download，Range）
 *     -> 平台 MD5 校验 -> 上报 step=100 -> 置 VALID -> 翻转活动分区 -> 软复位
 *  4. 无任务 -> 连接 MQTT 订阅 $sys/.../ota/inform 等待平台推送升级通知；
 *     收到通知回 inform_reply 后重新进入 HTTP 阶段；同时支持周期轮询兜底
 *
 * 双校验链：
 *  - 平台侧：下载完成后用平台下发的 MD5 校验（防止传输损坏）
 *  - 本机侧：下载同时计算 SHA-256 存入参数区，BootLoader 启动前二次校验并回滚
 ******************************************************************************
 */
#include "ota.h"
#include "ota_config.h"
#include "partition.h"
#include "ota_params.h"
#include "param_area.h"
#include "flash_if.h"
#include "sha256.h"
#include "md5.h"
#include "esp01s.h"
#include "http_client.h"
#include "mqtt_client.h"
#include "w25q16.h"
#include "usart.h"
#include "ota_ui.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* 看门狗句柄（只用于刷新，Instance 足够） */
static IWDG_HandleTypeDef s_iwdg = {.Instance = IWDG};

/* ---- 状态快照 ---- */
static ota_status_t s_status;
static volatile uint32_t s_check_requested = 0;   /* UI 手动触发检查 */
static volatile int    s_abort_report = 0;        /* 下载中触发进度上报（截断当前连接） */
static volatile uint32_t s_inform_received = 0;   /* 收到平台 OTA 通知 */

/* ---- 平台检测到的升级任务 ---- */
typedef struct {
    uint32_t tid;          /* 任务ID */
    uint32_t size;         /* 升级包字节数 */
    uint32_t type;         /* 1=完整包 2=差分包 */
    uint32_t status;       /* 1=待升级 2=下载中 3=升级中 */
    char     target[24];   /* 目标版本号字符串 */
    char     md5[40];      /* 升级包 MD5（小写十六进制 32 字符） */
    int      has_task;
} ota_task_t;

/* ---- 下载上下文 ---- */
typedef struct {
    uint32_t offset;
    uint32_t total;
    uint32_t last_save;
    uint32_t backup_addr;
    uint32_t tid;
    uint32_t report_thr;     /* 下一次进度上报阈值（百分比） */
    sha256_ctx_t sha;
    md5_ctx_t    md5;
    uint8_t  write_failed;   /* Flash 写入失败标志（停止继续下载） */
    uint8_t  pend[4];        /* 4 字节对齐缓冲：网络读取长度不受控，必须攒满 32 位字再编程 */
    uint8_t  pend_len;       /* 缓冲内有效字节数（0-3） */
} dl_ctx_t;

/* ---- 内部函数 ---- */
static int  json_get_str(const char *json, const char *key, char *out, uint32_t outlen);
static int  json_get_num(const char *json, const char *key, uint32_t *val);
static int  version_parse(const char *s, uint32_t *ver);
static int  md5_hex_equal(const char *hex, const uint8_t *digest);
static int  ota_api_report_version(void);
static int  ota_api_check_task(ota_task_t *t);
static int  ota_api_report_status(uint32_t tid, uint32_t step);
static void dl_on_data(const uint8_t *data, uint32_t len, void *arg);
static int  ota_do_download(const ota_task_t *task);
static int  ota_verify_backup_sha(const ota_param_t *p);
static void ota_apply_and_reboot(uint32_t tid);
static void ota_http_phase(void);

/* ================= 看门狗 ================= */

/* 弱符号：在 ESP01S 阻塞等待期间被调用，避免长阻塞时看门狗复位 */
void esp01s_wait_hook(void)
{
    ota_feed_watchdog();
}

void ota_feed_watchdog(void)
{
    HAL_IWDG_Refresh(&s_iwdg);
}

/* ================= 状态查询 ================= */

void ota_get_status(ota_status_t *st)
{
    memcpy(st, (void *)&s_status, sizeof(ota_status_t));
}

void ota_request_check(void)
{
    s_check_requested = 1;
}

/* ================= 恢复区初始化 ================= */

/* 恢复区初始化：如果恢复区是空的，把当前 A 槽固件拷贝到恢复区（出厂固件） */
static void ota_init_recovery_if_empty(void)
{
    uint8_t buf[16];
    
    /* 读恢复区前 16 字节 */
    w25q16_read(EXT_RECOVERY_ADDR, buf, sizeof(buf));
    
    /* 如果全是 0xFF，说明是空的，需要初始化 */
    int empty = 1;
    for (int i = 0; i < (int)sizeof(buf); i++)
    {
        if (buf[i] != 0xFF)
        {
            empty = 0;
            break;
        }
    }
    
    /* 如果已经初始化过了，就直接返回 */
    ota_param_t p;
    param_area_load(&p);
    if (!empty && p.recovery_len > 0) return;
    
    USART1_Printf("[OTA] recovery empty, init factory fw...\r\n");
    
    /* 自动检测实际固件大小：从 A 槽末尾往前找第一个非 0xFF 的字节 */
    uint32_t fw_size = OTA_APP_IMAGE_MAX_SIZE;
    uint8_t chk;
    for (int32_t off = OTA_APP_IMAGE_MAX_SIZE - 1; off >= 0; off--)
    {
        flash_if_read(OTA_APP_A_ADDR + off, &chk, 1);
        if (chk != 0xFF)
        {
            fw_size = (uint32_t)off + 1;
            break;
        }
        ota_feed_watchdog();
    }
    
    /* 四舍五入到 4KB 对齐（Flash 擦除按扇区） */
    fw_size = (fw_size + 0xFFFU) & ~0xFFFU;
    if (fw_size > OTA_APP_IMAGE_MAX_SIZE) fw_size = OTA_APP_IMAGE_MAX_SIZE;
    
    USART1_Printf("[OTA] detected fw size: %lu bytes\r\n", (unsigned long)fw_size);
    
    /* 擦除整个恢复区 */
    for (uint32_t off = 0; off < OTA_APP_IMAGE_MAX_SIZE; off += 65536)
    {
        w25q16_erase_block(EXT_RECOVERY_ADDR + off);
        ota_feed_watchdog();
    }
    
    /* 把当前 A 槽固件拷贝到恢复区，同时算 SHA-256 */
    uint32_t remain = fw_size;
    uint8_t rbuf[1024];
    uint32_t copied = 0;
    uint8_t recovery_digest[SHA256_DIGEST_SIZE];
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    while (remain > 0)
    {
        uint32_t take = (remain > sizeof(rbuf)) ? sizeof(rbuf) : remain;
        flash_if_read(OTA_APP_A_ADDR + copied, rbuf, take);
        w25q16_write(EXT_RECOVERY_ADDR + copied, rbuf, take);
        sha256_update(&ctx, rbuf, take);
        copied += take;
        remain -= take;
        ota_feed_watchdog();
    }
    sha256_final(&ctx, recovery_digest);
    
    /* 把实际固件大小和 SHA-256 写到参数区 */
    if (param_area_load(&p) == 0)
    {
        p.recovery_len = fw_size;
        memcpy(p.recovery_sha, recovery_digest, sizeof(recovery_digest));
        param_area_save(&p);
    }
    
    USART1_Printf("[OTA] recovery init done (%lu bytes)\r\n", (unsigned long)fw_size);
}

/* ================= 手动回滚 / 恢复出厂 ================= */

/* 手动回滚到上一版本 */
void ota_manual_rollback(void)
{
    ota_param_t p;
    if (param_area_load(&p) != 0) return;
    if (p.backup_status != OTA_BACKUP_VALID) return;  /* 没有备份就不能回滚 */
    
    p.boot_flag = OTA_BOOT_FLAG_ROLLBACK;
    p.boot_count = 0;
    param_area_save(&p);
    
    /* 重启 */
    NVIC_SystemReset();
}

/* 手动恢复出厂设置 */
void ota_manual_recovery(void)
{
    ota_param_t p;
    param_area_load(&p);
    
    p.boot_flag = OTA_BOOT_FLAG_RECOVERY;
    p.boot_count = 0;
    param_area_save(&p);
    
    /* 重启 */
    NVIC_SystemReset();
}

/* ================= 启动成功确认 ================= */

void ota_confirm_boot_success(void)
{
    ota_param_t p;

    if (param_area_load(&p) != 0)
    {
        return;
    }
    
    /* 版本号不匹配时，更新版本号（回滚/恢复出厂后版本号会变） */
    int need_save = 0;
    if (p.version != OTA_APP_VERSION_NUM)
    {
        p.version = OTA_APP_VERSION_NUM;
        need_save = 1;
    }

    if (p.boot_flag != OTA_BOOT_FLAG_TRY_NEW)
    {
        /* 不是新固件验证，只更新版本号就行 */
        if (need_save) param_area_save(&p);
        return;
    }

    /* 新固件正常运行满窗口：固化成功标志（保留 ota_tid，稍后补报 201） */
    p.boot_flag      = OTA_BOOT_FLAG_NORMAL;
    p.boot_count     = 0;
    p.version        = OTA_APP_VERSION_NUM;
    p.resume_offset  = 0;
    p.backup_status  = OTA_BACKUP_VALID;   /* 另一槽位为旧固件，视为有效备份 */
    p.image_len      = 0;                  /* 旧固件长度未知，清空哈希记录 */
    memset(p.image_sha, 0, sizeof(p.image_sha));
    p.dl_tid         = 0;
    param_area_save(&p);
}

/* ================= JSON 极小解析 ================= */

static int json_get_str(const char *json, const char *key, char *out, uint32_t outlen)
{
    char pat[48];
    const char *p;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (p == NULL) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    {
        uint32_t n = 0;
        while (*p && *p != '"' && n < outlen - 1U)
        {
            out[n++] = *p++;
        }
        out[n] = '\0';
        return (*p == '"') ? 0 : -1;
    }
}

/* 取裸数值（整数），如 "tid": 123 */
static int json_get_num(const char *json, const char *key, uint32_t *val)
{
    char pat[48];
    const char *p;
    char buf[24];
    uint32_t n = 0;

    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (p == NULL) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;

    while (*p && *p != ',' && *p != '}' && *p != ']' &&
           *p != '"' && *p != ' ' && *p != '\t' && n < sizeof(buf) - 1U)
    {
        buf[n++] = *p++;
    }
    buf[n] = '\0';
    if (n == 0)
    {
        return -1;
    }
    *val = (uint32_t)strtoul(buf, NULL, 10);
    return 0;
}

/* "1.2.3" -> 0x010203 */
static int version_parse(const char *s, uint32_t *ver)
{
    unsigned a, b, c;
    if (sscanf(s, "%u.%u.%u", &a, &b, &c) != 3)
    {
        return -1;
    }
    *ver = OTA_VERSION(a, b, c);
    return 0;
}

/* 平台 MD5 十六进制串（大小写不敏感）与摘要比较 */
static int md5_hex_equal(const char *hex, const uint8_t *digest)
{
    char s[MD5_DIGEST_SIZE * 2 + 1];
    int i;

    if (hex == NULL || strlen(hex) != (MD5_DIGEST_SIZE * 2U))
    {
        return 0;
    }
    md5_digest_to_hex(digest, s);
    for (i = 0; i < MD5_DIGEST_SIZE * 2; i++)
    {
        char a = hex[i];
        if (a >= 'A' && a <= 'F')
        {
            a = (char)(a - 'A' + 'a');
        }
        if (a != s[i])
        {
            return 0;
        }
    }
    return 1;
}

/* ================= OneNET 南向 OTA API ================= */

/* 上报当前固件版本（best-effort） */
static int ota_api_report_version(void)
{
    char path[160];
    char body[80];
    char extra[200];
    uint8_t rbuf[192];
    http_resp_t resp;

    snprintf(path, sizeof(path), "/fuse-ota/%s/%s/version", OTA_MQTT_USER, OTA_DEVICE_ID);
    snprintf(body, sizeof(body), "{\"s_version\":\"%s\",\"f_version\":\"%s\"}",
             OTA_APP_VERSION_STR, OTA_FW_VERSION_STR);
    snprintf(extra, sizeof(extra), "Content-Type: application/json\r\nAuthorization: %s\r\n",
             OTA_HTTP_AUTH);

    memset(&resp, 0, sizeof(resp));
    resp.body = rbuf;
    resp.body_cap = sizeof(rbuf);
    return http_client_request(OTA_API_HOST, OTA_API_PORT, "POST", path, extra, body,
                               &resp, NULL, NULL, 5000);
}

/* 检测升级任务；无任务时 t->has_task=0 */
static int ota_api_check_task(ota_task_t *t)
{
    char path[200];
    char extra[200];
    uint8_t rbuf[512];
    http_resp_t resp;
    uint32_t code;
    uint32_t v;

    memset(t, 0, sizeof(*t));

    snprintf(path, sizeof(path), "/fuse-ota/%s/%s/check?type=%d&version=%s",
             OTA_MQTT_USER, OTA_DEVICE_ID, OTA_TASK_TYPE, OTA_APP_VERSION_STR);
    snprintf(extra, sizeof(extra), "Authorization: %s\r\n", OTA_HTTP_AUTH);

    memset(&resp, 0, sizeof(resp));
    resp.body = rbuf;
    resp.body_cap = sizeof(rbuf);
    if (http_client_request(OTA_API_HOST, OTA_API_PORT, "GET", path, extra, NULL,
                            &resp, NULL, NULL, 5000) != ESP_OK)
    {
        return -1;
    }

    rbuf[resp.body_len < sizeof(rbuf) ? resp.body_len : sizeof(rbuf) - 1U] = '\0';

    if (json_get_num((const char *)rbuf, "code", &code) != 0 || code != 0)
    {
        return 0;   /* 平台返回错误或网络异常：按无任务处理 */
    }

    if (json_get_num((const char *)rbuf, "tid", &v) != 0 || v == 0)
    {
        return 0;   /* 无 data / 无任务 */
    }
    t->tid = v;
    t->has_task = 1;

    if (json_get_num((const char *)rbuf, "size", &v) == 0) t->size = v;
    if (json_get_num((const char *)rbuf, "type", &v) == 0) t->type = v;
    if (json_get_num((const char *)rbuf, "status", &v) == 0) t->status = v;
    if (json_get_str((const char *)rbuf, "target", t->target, sizeof(t->target)) != 0)
    {
        t->target[0] = '\0';
    }
    if (json_get_str((const char *)rbuf, "md5", t->md5, sizeof(t->md5)) != 0)
    {
        t->md5[0] = '\0';
    }
    return 0;
}

/* 上报下载进度（step 0-100）或升级状态（step>100，如 201） */
static int ota_api_report_status(uint32_t tid, uint32_t step)
{
    char path[200];
    char body[32];
    char extra[200];
    uint8_t rbuf[192];
    http_resp_t resp;
    uint32_t code;

    snprintf(path, sizeof(path), "/fuse-ota/%s/%s/%lu/status",
             OTA_MQTT_USER, OTA_DEVICE_ID, (unsigned long)tid);
    snprintf(body, sizeof(body), "{\"step\":%lu}", (unsigned long)step);
    snprintf(extra, sizeof(extra), "Content-Type: application/json\r\nAuthorization: %s\r\n",
             OTA_HTTP_AUTH);

    memset(&resp, 0, sizeof(resp));
    resp.body = rbuf;
    resp.body_cap = sizeof(rbuf);
    if (http_client_request(OTA_API_HOST, OTA_API_PORT, "POST", path, extra, body,
                            &resp, NULL, NULL, 5000) != ESP_OK)
    {
        return -1;
    }

    rbuf[resp.body_len < sizeof(rbuf) ? resp.body_len : sizeof(rbuf) - 1U] = '\0';
    if (json_get_num((const char *)rbuf, "code", &code) != 0 || code != 0)
    {
        return -1;
    }
    return 0;
}

/* ================= 下载 ================= */

static void dl_on_data(const uint8_t *data, uint32_t len, void *arg)
{
    dl_ctx_t *c = (dl_ctx_t *)arg;
    ota_param_t p;

    if (c->write_failed)
    {
        return;   /* 已发生写入失败，丢弃后续数据 */
    }

    /* 哈希基于原始字节流计算（与平台 MD5 保持一致，不含任何填充） */
    sha256_update(&c->sha, data, len);
    md5_update(&c->md5, data, len);

    /* ---- 4 字节对齐写入：网络读取长度不受控，必须先攒满 32 位字再编程，
       否则 c->offset 会变成非 4 对齐，HAL_FLASH_Program 在非对齐地址必失败 ---- */

    /* 1) 先拼入上次残留，凑满 4 字节立即写入 */
    if (c->pend_len > 0)
    {
        uint32_t need = 4U - c->pend_len;
        uint32_t take = (len < need) ? len : need;
        memcpy(c->pend + c->pend_len, data, take);
        c->pend_len += (uint8_t)take;
        data += take;
        len  -= take;
        if (c->pend_len == 4U)
        {
            if (w25q16_write(c->backup_addr + c->offset, c->pend, 4U) != HAL_OK)
            {
                c->write_failed = 1;
                s_status.state = OTA_STATE_ERROR;
                snprintf(s_status.msg, sizeof(s_status.msg), "ext flash write err @%lu", (unsigned long)c->offset);
                return;
            }
            c->offset += 4U;
            c->pend_len = 0;
        }
    }

    /* 2) 主体：整字批量写入（截断到 4 的倍数；offset 始终 4 对齐） */
    if (len >= 4U)
    {
        uint32_t n = len & ~3U;
        if (w25q16_write(c->backup_addr + c->offset, data, n) != HAL_OK)
        {
            c->write_failed = 1;
            s_status.state = OTA_STATE_ERROR;
            snprintf(s_status.msg, sizeof(s_status.msg), "ext flash write err @%lu", (unsigned long)c->offset);
            return;
        }
        c->offset += n;
        data += n;
        len  -= n;
    }

    /* 3) 尾部 0-3 字节缓存到下轮（offset 不变，仍 4 对齐） */
    if (len > 0)
    {
        memcpy(c->pend, data, len);
        c->pend_len = (uint8_t)len;
    }

    /* 每 16KB 写一次断点检查点 */
    if (c->offset - c->last_save >= OTA_CHECKPOINT_INTERVAL)
    {
        if (param_area_load(&p) == 0)
        {
            p.resume_offset = c->offset;
            p.backup_status = OTA_BACKUP_DOWNLOADING;
            p.dl_tid        = c->tid;
            param_area_save(&p);
        }
        c->last_save = c->offset;
    }

    /* 进度（同步到 UI 状态） */
    if (c->total > 0)
    {
        uint32_t pct = (uint32_t)((uint64_t)c->offset * 100U / c->total);
        s_status.progress = pct;
        s_status.downloaded = c->offset;
        /* 跨过进度上报阈值且未到 100%：置标志，由主流程截断连接后上报
           （避免 ESP 单连接冲突）；100% 即下载完成，让当前连接自然结束 */
        if (c->tid != 0 && pct < 100U && pct >= c->report_thr)
        {
            s_abort_report = (int)pct;
            c->report_thr = pct + OTA_PROGRESS_STEP;
        }
    }
}

static int ota_do_download(const ota_task_t *task)
{
    ota_param_t p;
    dl_ctx_t c;
    http_resp_t resp;
    char path[200];
    char extra[240];
    uint8_t md5_digest[MD5_DIGEST_SIZE];
    uint8_t sha_digest[SHA256_DIGEST_SIZE];
    uint32_t start_offset;
    int r;
    int resume_ok;

    /* ---- 暂存区固定为外部 W25Q16：
       BootLoader 在 TRY_NEW 启动时把外部暂存区校验后拷贝到片内 A ---- */
    c.backup_addr = EXT_STAGING_ADDR;
    c.tid         = task->tid;
    c.total       = task->size;
    c.write_failed = 0;
    c.pend_len    = 0;
    c.report_thr  = OTA_PROGRESS_STEP;
    sha256_init(&c.sha);
    md5_init(&c.md5);
    s_abort_report = 0;

    /* ---- 断点检查：同一任务 + 下载中才续传；否则全新下载 ---- */
    resume_ok = 0;
    if (param_area_load(&p) == 0 &&
        p.dl_tid == task->tid && p.backup_status == OTA_BACKUP_DOWNLOADING &&
        (p.resume_offset & 3U) == 0U)   /* 旧版本可能留下非 4 对齐断点，无法安全续传，强制全新下载 */
    {
        resume_ok = 1;
    }
    if (!resume_ok)
    {
        /* 擦除外部 Flash 暂存区（448KB = 7 个 64KB 块） */
        for (uint32_t off = 0; off < OTA_APP_IMAGE_MAX_SIZE; off += 65536)
        {
            w25q16_erase_block(EXT_STAGING_ADDR + off);
        }
        c.offset = 0;
        c.last_save = 0;
        if (param_area_load(&p) == 0)
        {
            p.resume_offset = 0;
            p.backup_status = OTA_BACKUP_DOWNLOADING;
            p.dl_tid        = task->tid;
            p.image_len     = 0;
            memset(p.image_sha, 0, sizeof(p.image_sha));
            param_area_save(&p);
        }
    }
    else
    {
        c.offset = p.resume_offset;
        c.last_save = c.offset;
    }

    s_status.state = OTA_STATE_DOWNLOADING;
    snprintf(s_status.msg, sizeof(s_status.msg), "downloading...");
    s_status.progress = (c.total > 0) ? (uint32_t)((uint64_t)c.offset * 100U / c.total) : 0;
    s_status.total = c.total;

    /* ---- 下载主循环：进度上报会截断连接（每 10% 一次），轮数放宽到 32 ---- */
    {
        int done = 0;
        int attempt;

        for (attempt = 0; attempt < 32 && !done; attempt++)
        {
            start_offset = c.offset;

            snprintf(path, sizeof(path), "/fuse-ota/%s/%s/%lu/download",
                     OTA_MQTT_USER, OTA_DEVICE_ID, (unsigned long)task->tid);
            if (start_offset > 0)
            {
                snprintf(extra, sizeof(extra),
                         "Authorization: %s\r\nRange: bytes=%lu-\r\n",
                         OTA_HTTP_AUTH, (unsigned long)start_offset);
            }
            else
            {
                snprintf(extra, sizeof(extra), "Authorization: %s\r\n", OTA_HTTP_AUTH);
            }

            memset(&resp, 0, sizeof(resp));
            resp.abort_flag = &s_abort_report;

            r = http_client_request(OTA_API_HOST, OTA_API_PORT, "GET", path, extra, NULL,
                                    &resp, dl_on_data, &c, 5000);

            /* ---- 进度上报截断：保存检查点，上报后继续续传 ---- */
            if (s_abort_report != 0)
            {
                uint32_t step = (uint32_t)s_abort_report;
                s_abort_report = 0;
                if (param_area_load(&p) == 0)
                {
                    p.resume_offset = c.offset;
                    p.backup_status = OTA_BACKUP_DOWNLOADING;
                    p.dl_tid        = task->tid;
                    param_area_save(&p);
                }
                (void)ota_api_report_status(task->tid, step);   /* best-effort */
                continue;
            }

            if (r == ESP_OK)
            {
                /* OneNET 下载接口的业务错误码（Ota-Errno > 0 表示任务/鉴权异常） */
                if (resp.ota_errno > 0)
                {
                    snprintf(s_status.msg, sizeof(s_status.msg), "ota errno %d", resp.ota_errno);
                    s_status.state = OTA_STATE_ERROR;
                    (void)ota_api_report_status(task->tid, 207);
                    return -1;
                }
                /* 416：Range 起点等于文件总长，说明此前已下载完整（如 100% 截断后
                   断点保存为 total），按"已全部接收"处理，交由 MD5/大小校验把关 */
                if (resp.status_code == 416 && c.offset >= task->size)
                {
                    c.total = c.offset;
                    done = 1;
                    break;
                }
                if (resp.total_size > 0)
                {
                    c.total = resp.total_size;
                }
                else
                {
                    c.total = c.offset;   /* 无 Content-Range 时以实际接收为准 */
                }
                s_status.total = c.total;
                done = 1;
                break;
            }

            /* Flash 写入失败：数据已不可信，直接退出 */
            if (c.write_failed)
            {
                return -1;
            }

            /* 请求了 Range 却返回 200 全量：从头重下 */
            if (attempt < 31 && start_offset > 0 &&
                resp.status_code == HTTP_STATUS_OK && c.offset == start_offset)
            {
                if (flash_if_erase_region(c.backup_addr, OTA_APP_IMAGE_MAX_SIZE) != HAL_OK)
                {
                    return -1;
                }
                c.offset = 0;
                c.last_save = 0;
                if (param_area_load(&p) == 0)
                {
                    p.resume_offset = 0;
                    p.backup_status = OTA_BACKUP_DOWNLOADING;
                    p.dl_tid        = task->tid;
                    param_area_save(&p);
                }
                continue;
            }

            /* 网络中断：保存检查点，退出等待下次续传 */
            if (param_area_load(&p) == 0)
            {
                p.resume_offset = c.offset;
                p.backup_status = OTA_BACKUP_DOWNLOADING;
                p.dl_tid        = task->tid;
                param_area_save(&p);
            }
            snprintf(s_status.msg, sizeof(s_status.msg), "net err, resume @%lu", (unsigned long)c.offset);
            s_status.state = OTA_STATE_ERROR;
            (void)ota_api_report_status(task->tid, 104);   /* 下载请求超时 */
            return -1;
        }

        if (!done)
        {
            /* 轮数耗尽仍未完成（异常）：保存检查点，下次续传 */
            if (param_area_load(&p) == 0)
            {
                p.resume_offset = c.offset;
                p.backup_status = OTA_BACKUP_DOWNLOADING;
                p.dl_tid        = task->tid;
                param_area_save(&p);
            }
            snprintf(s_status.msg, sizeof(s_status.msg), "download interrupted");
            s_status.state = OTA_STATE_ERROR;
            return -1;
        }
    }

    /* ---- 收尾：不足 4 字节的尾部补齐 0xFF 写入（offset 对齐；固件为 4 的倍数时通常为空） ---- */
    if (c.pend_len > 0)
    {
        uint8_t tail[4] = {0xFFU, 0xFFU, 0xFFU, 0xFFU};
        memcpy(tail, c.pend, c.pend_len);
        if (w25q16_write(c.backup_addr + c.offset, tail, 4U) != HAL_OK)
        {
            snprintf(s_status.msg, sizeof(s_status.msg), "ext flash write err @%lu", (unsigned long)c.offset);
            s_status.state = OTA_STATE_ERROR;
            return -1;
        }
        c.offset += 4U;
        c.pend_len = 0;
    }

    /* ---- 完整性校验 ---- */
    s_status.state = OTA_STATE_VERIFYING;
    snprintf(s_status.msg, sizeof(s_status.msg), "verify md5...");

    /* 平台 MD5 校验 */
    md5_final(&c.md5, md5_digest);
    if (task->md5[0] != '\0' && !md5_hex_equal(task->md5, md5_digest))
    {
        snprintf(s_status.msg, sizeof(s_status.msg), "MD5 mismatch");
        s_status.state = OTA_STATE_ERROR;
        (void)ota_api_report_status(task->tid, 205);   /* MD5 校验失败 */
        return -1;
    }

    /* 大小一致性校验（与平台任务中的 size 对比） */
    if (task->size > 0 && c.total != task->size)
    {
        snprintf(s_status.msg, sizeof(s_status.msg), "size mismatch");
        s_status.state = OTA_STATE_ERROR;
        (void)ota_api_report_status(task->tid, 206);
        return -1;
    }

    /* 计算 SHA-256 供 BootLoader 二次校验 */
    sha256_final(&c.sha, sha_digest);

    /* ---- 备份旧版本到外部 Flash 备份区 ---- */
    s_status.state = OTA_STATE_VERIFYING;
    snprintf(s_status.msg, sizeof(s_status.msg), "backup old fw...");

    /* 先读参数区，拿到旧固件大小（用于备份和记录 backup_len） */
    ota_param_t old_param;
    uint32_t old_image_len = 0;
    uint32_t old_version = 0;
    if (param_area_load(&old_param) == 0)
    {
        old_image_len = old_param.image_len;
        old_version = old_param.version;
    }

    /* 旧固件大小无效的话，用整个 A 槽大小 */
    if (old_image_len == 0 || old_image_len > OTA_APP_IMAGE_MAX_SIZE)
    {
        old_image_len = OTA_APP_IMAGE_MAX_SIZE;
    }

    /* 擦除外部备份区 */
    for (uint32_t off = 0; off < OTA_APP_IMAGE_MAX_SIZE; off += 65536)
    {
        w25q16_erase_block(EXT_BACKUP_ADDR + off);
    }

    /* 把片内 A 槽的旧固件拷贝到外部备份区，同时算 SHA-256 */
    uint8_t backup_digest[SHA256_DIGEST_SIZE];
    {
        uint32_t remain = old_image_len;
        uint8_t buf[1024];
        uint32_t copied = 0;
        sha256_ctx_t ctx;
        sha256_init(&ctx);
        while (remain > 0)
        {
            uint32_t take = (remain > sizeof(buf)) ? sizeof(buf) : remain;
            flash_if_read(OTA_APP_A_ADDR + copied, buf, take);
            w25q16_write(EXT_BACKUP_ADDR + copied, buf, take);
            sha256_update(&ctx, buf, take);
            copied += take;
            remain -= take;
            ota_feed_watchdog();
        }
        sha256_final(&ctx, backup_digest);
    }

    /* ---- 固化升级信息 ---- */
    if (param_area_load(&p) == 0)
    {
        p.image_len     = c.total;
        p.backup_version = old_version;  /* 记录备份的旧版本号 */
        p.backup_len    = old_image_len; /* 记录备份的旧固件大小 */
        memcpy(p.image_sha, sha_digest, sizeof(sha_digest));
        memcpy(p.backup_sha, backup_digest, sizeof(backup_digest));
        p.backup_status = OTA_BACKUP_VALID;
        p.resume_offset = 0;
        p.dl_tid        = task->tid;
        param_area_save(&p);
    }

    /* 上报下载完成（step=100，平台置为"正在升级"） */
    (void)ota_api_report_status(task->tid, 100);

    s_status.progress = 100;
    s_status.total = c.total;
    s_status.downloaded = c.total;
    return 0;
}

/* 校验外部暂存区镜像 SHA-256 与参数区记录一致（用于跳过重复下载） */
static int ota_verify_backup_sha(const ota_param_t *p)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint32_t remaining = p->image_len;
    uint8_t chunk[64];
    sha256_ctx_t ctx;

    if (p->backup_status != OTA_BACKUP_VALID || p->image_len == 0 ||
        p->image_len > OTA_APP_IMAGE_MAX_SIZE)
    {
        return -1;
    }

    sha256_init(&ctx);
    while (remaining > 0)
    {
        uint32_t take = (remaining > sizeof(chunk)) ? (uint32_t)sizeof(chunk) : remaining;
        w25q16_read(EXT_STAGING_ADDR + (p->image_len - remaining), chunk, take);
        sha256_update(&ctx, chunk, take);
        remaining -= take;
        ota_feed_watchdog();
    }
    sha256_final(&ctx, digest);
    return sha256_equal(digest, p->image_sha) ? 0 : -1;
}

/* ================= 重启切换 ================= */

static void ota_apply_and_reboot(uint32_t tid)
{
    ota_param_t p;
    int i;

    if (param_area_load(&p) != 0)
    {
        param_area_defaults(&p);
    }

    s_status.state = OTA_STATE_CONFIRM_REBOOT;
    snprintf(s_status.msg, sizeof(s_status.msg), "update ok, reboot in 5s");

    for (i = 5; i > 0; i--)
    {
        ota_feed_watchdog();
        snprintf(s_status.msg, sizeof(s_status.msg), "reboot in %ds", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* 置 TRY_NEW 标志：BootLoader 下次启动把暂存槽 B 的新镜像拷贝到
       运行槽 A 后启动；ota_tid 记录本任务，待新固件运行成功后补报 201。
       （运行槽固定为 A，不翻转 active_slot） */
    p.boot_flag     = OTA_BOOT_FLAG_TRY_NEW;
    p.boot_count    = 0;
    p.ota_tid       = tid;
    p.dl_tid        = 0;
    param_area_save(&p);

    mqtt_disconnect();
    ota_feed_watchdog();
    HAL_Delay(50);
    NVIC_SystemReset();
}

/* ================= MQTT 通知 ================= */

/*
 * OneNET 系统 OTA 升级通知（下行）示例：
 *   {"id":"123","version":"1.0","params":["xxxxxxx-OTA-C"]}
 * 设备需在 20s 内回复 inform_reply：
 *   {"id":"123","code":200,"msg":"ok"}
 */
static void on_mqtt_publish(const char *topic, const uint8_t *payload, uint16_t len, void *arg)
{
    char json[192];
    char id[24];
    char reply[96];

    (void)arg;
    if (strcmp(topic, OTA_TOPIC_INFORM) != 0)
    {
        return;
    }
    if (len >= sizeof(json))
    {
        len = sizeof(json) - 1U;
    }
    memcpy(json, payload, len);
    json[len] = '\0';

    /* 取出通知 id 用于应答 */
    if (json_get_str(json, "id", id, sizeof(id)) != 0)
    {
        snprintf(id, sizeof(id), "0");
    }
    snprintf(reply, sizeof(reply), "{\"id\":\"%s\",\"code\":200,\"msg\":\"ok\"}", id);
    mqtt_publish(OTA_TOPIC_INFORM_REPLY, (const uint8_t *)reply, (uint16_t)strlen(reply), 2000);

    s_inform_received = 1;   /* 主流程收到后退出 MQTT 等待，进入 HTTP 检测阶段 */
}

/* ================= HTTP 阶段（检测/下载/状态上报） ================= */

static void ota_http_phase(void)
{
    ota_param_t p;
    ota_task_t t;
    uint32_t cur_ver;

    /* 1. 补报 201：仅当当前运行版本为本构建版本（说明新固件已成功运行） */
    if (param_area_load(&p) == 0 && p.ota_tid != 0 && p.version == OTA_APP_VERSION_NUM)
    {
        if (ota_api_report_status(p.ota_tid, 201) == 0)
        {
            p.ota_tid = 0;
            param_area_save(&p);
            snprintf(s_status.msg, sizeof(s_status.msg), "upgrade reported");
        }
    }

    /* 2. 上报当前版本（平台记录设备当前版本，用于匹配升级任务） */
    (void)ota_api_report_version();

    /* 3. 检测升级任务
       [FIX-12] 区分"网络/接口失败"与"确实无任务"：
       HTTP 请求失败（返回 -1）进入 ERROR 退避重试，避免误报
       "no task" 后立刻去连 MQTT（此时网络本来就是断的，
       造成 mqtt fail 与 esp init fail 连环失败的假象）。
       [FIX-20] 失败后再区分"WiFi 掉了"与"纯网络失败"：
       WiFi 掉线时立即回 CONNECTING 重连（跳过 3s~24s 退避空转）。 */
    if (ota_api_check_task(&t) != 0)
    {
        /* [FIX-21] 只有应答明确"未连接"（+CWJAP:0/1/2/4 或 ERROR）才重连；
           ESP_TIMEOUT（AT 引擎忙、查询无人应答）不代表 WiFi 掉了，
           按原逻辑退避重试，避免"查询超时→全量重连"的 ~7s 死循环。 */
        if (esp01s_wifi_status(2000) == ESP_ERR)
        {
            s_status.state = OTA_STATE_CONNECTING;
            snprintf(s_status.msg, sizeof(s_status.msg), "wifi lost, reconnect");
            return;
        }
        s_status.state = OTA_STATE_ERROR;
        snprintf(s_status.msg, sizeof(s_status.msg), "check fail, retry");
        return;
    }
    if (!t.has_task)
    {
        s_status.state = OTA_STATE_WAIT_NOTIFY;
        snprintf(s_status.msg, sizeof(s_status.msg), "no task, waiting...");
        return;
    }

    /* 任务校验：差分包不支持 / 目标版本防降级 */
    if (t.type != 1)
    {
        s_status.state = OTA_STATE_ERROR;
        snprintf(s_status.msg, sizeof(s_status.msg), "diff pkg unsupported");
        (void)ota_api_report_status(t.tid, 206);
        return;
    }
    if (t.target[0] != '\0' && version_parse(t.target, &cur_ver) == 0)
    {
        if (param_area_load(&p) != 0)
        {
            param_area_defaults(&p);
        }
        if (cur_ver <= p.version)
        {
            if (cur_ver == p.version)
            {
                /* 平台任务目标版本与当前运行版本相同：说明升级已完成但
                   201 尚未被平台接受（如 201 早于平台状态更新），补报一次 */
                (void)ota_api_report_status(t.tid, 201);
                s_status.state = OTA_STATE_WAIT_NOTIFY;
                snprintf(s_status.msg, sizeof(s_status.msg), "already updated");
                return;
            }
            s_status.state = OTA_STATE_ERROR;
            snprintf(s_status.msg, sizeof(s_status.msg), "rejected: ver too low");
            (void)ota_api_report_status(t.tid, 206);
            return;
        }
    }

    /* 已下载过同一任务且备份完整：校验 SHA 后直接应用，避免重复下载 */
    if (param_area_load(&p) == 0 &&
        p.dl_tid == t.tid && p.backup_status == OTA_BACKUP_VALID &&
        p.image_len == t.size && ota_verify_backup_sha(&p) == 0)
    {
        snprintf(s_status.msg, sizeof(s_status.msg), "image ready, apply");
        (void)ota_api_report_status(t.tid, 100);   /* 补报下载完成 */
        ota_apply_and_reboot(t.tid);
        return;   /* 不返回 */
    }

    /* 全新下载 */
    if (ota_do_download(&t) == 0)
    {
        ota_apply_and_reboot(t.tid);
    }
}

/* ================= 失败退避 ================= */

/* [FIX-11] 连续失败退避：3s -> 6s -> 12s -> 24s（上限 24s），
   任一环节成功即清零。防止模块/网络异常时以 3s 周期无限空转刷串口，
   也给 ESP 模块（如供电不足自恢复）留出恢复时间。 */
static uint32_t s_fail_cnt = 0;

static uint32_t ota_backoff_ms(void)
{
    uint32_t shift = (s_fail_cnt > 3U) ? 3U : s_fail_cnt;
    return 3000U << shift;   /* 3000, 6000, 12000, 24000 */
}

static void ota_fail_wait(void)
{
    uint32_t ms = ota_backoff_ms();
    if (s_fail_cnt < 0xFFFFFFF0U)
    {
        s_fail_cnt++;
    }
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void ota_success_reset(void)
{
    s_fail_cnt = 0;
}

/* ================= OTA 任务 ================= */

void OTA_Task(void *pvParameters)
{
    ota_param_t p;
    uint32_t boot_start_tick = HAL_GetTick();
    uint32_t last_ping = 0;
    uint32_t last_confirm_tick = 0;
    int startup_pending = 0;
    char last_msg[64] = "";


    (void)pvParameters;

    /* ---- 恢复区初始化（如果是空的，写入当前固件作为出厂固件） ---- */
    ota_init_recovery_if_empty();

    /* ---- 开机即自动开始一次检测（上报版本/201、检测任务、连接等待通知） ---- */
    s_status.state = OTA_STATE_CONNECTING;

    /* ---- 若正处于 TRY_NEW（新固件首启），记录启动时刻 ---- */
    if (param_area_load(&p) == 0 && p.boot_flag == OTA_BOOT_FLAG_TRY_NEW)
    {
        startup_pending = 1;
        last_confirm_tick = boot_start_tick;
    }
	else
    {
        /* [FIX] 回滚/恢复出厂后 boot_flag 为 NORMAL，ota_confirm_boot_success()
           不会被调用，参数区 version 会停留在上一次升级的版本号，
           导致 About 页与 [BL] ver= 打印显示旧版本。这里启动时立即同步。 */
        ota_confirm_boot_success();   /* 非 TRY_NEW 时该函数只做版本号同步并返回 */
    }

    for (;;)
    {
        ota_feed_watchdog();
		
		/* ---- 状态变化时打印到串口（便于排查卡点；全部使用 ASCII 避免串口编码问题） ---- */
        if (strcmp(last_msg, s_status.msg) != 0)
        {
            strncpy(last_msg, s_status.msg, sizeof(last_msg) - 1U);
            last_msg[sizeof(last_msg) - 1U] = '\0';
            USART1_Printf("[OTA] %s\r\n", s_status.msg);
        }

        /* ---- 新固件首启：窗口期内不执行任何升级动作，静默等待成功确认 ---- */
        if (startup_pending &&
            (HAL_GetTick() - last_confirm_tick) < OTA_STARTUP_CONFIRM_MS)
        {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        /* ---- 启动成功确认（窗口 30s 到点） ---- */
        if (startup_pending)
        {
            ota_confirm_boot_success();
            startup_pending = 0;
            s_status.state = OTA_STATE_CONNECTING;
            snprintf(s_status.msg, sizeof(s_status.msg), "firmware OK");
        }

        /* ---- 空闲：等待手动触发或定时巡检 ---- */
        if (s_status.state == OTA_STATE_IDLE || s_status.state == OTA_STATE_ERROR)
        {
            if (s_check_requested == 0 && s_status.state == OTA_STATE_IDLE)
            {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            s_check_requested = 0;
            s_status.state = OTA_STATE_CONNECTING;
        }

        /* ---- 连接 WiFi ---- */
        if (s_status.state == OTA_STATE_CONNECTING)
        {
            int i;
            int ok = 0;
			
			/* 先初始化 ESP01S：AT 握手 + 单连接模式(CIPMUX=0)。
               缺失会导致模块状态未知（如残留多连接模式），
               AT+CIPSTART 无法建连，平台永远"未激活"。 */
            snprintf(s_status.msg, sizeof(s_status.msg), "esp init...");
            for (i = 0; i < 3; i++)
            {
                ota_feed_watchdog();
                if (esp01s_init() == ESP_OK)
                {
                    ok = 1;
                    break;
                }
                ota_feed_watchdog();
            }
            if (!ok)
            {
                /* [FIX-11] 失败退避；esp01s_init 内部已含 AT+RST 自恢复 */
                snprintf(s_status.msg, sizeof(s_status.msg), "esp init fail, retry");
                s_status.state = OTA_STATE_ERROR;
                ota_fail_wait();
                continue;
            }
            ota_success_reset();

            ok = 0;
            snprintf(s_status.msg, sizeof(s_status.msg), "wifi connecting...");

            for (i = 0; i < 3; i++)
            {
                ota_feed_watchdog();
                if (esp01s_join_ap(OTA_WIFI_SSID, OTA_WIFI_PASS, 10000) == ESP_OK)
                {
                    ok = 1;
                    break;
                }
                ota_feed_watchdog();
            }
            if (!ok)
            {
                snprintf(s_status.msg, sizeof(s_status.msg), "wifi fail, retry later");
                s_status.state = OTA_STATE_ERROR;
                ota_fail_wait();
                continue;
            }
            ota_success_reset();
            /* [FIX-18] WiFi 刚 GOT IP 后，模块网络栈与路由器 NAT 表
               需短暂稳定；立即建 TCP 实测得到半死连接（CONNECT OK 但
               SEND OK 后服务器零响应即 CLOSED）。等待 2s 稳定窗口。 */
            HAL_Delay(2000);
            ota_feed_watchdog();
            s_status.state = OTA_STATE_CHECKING;
            snprintf(s_status.msg, sizeof(s_status.msg), "checking...");
        }

        /* ---- HTTP 阶段（此时 MQTT 未连接，ESP 单连接无冲突） ---- */
        if (s_status.state == OTA_STATE_CHECKING || s_status.state == OTA_STATE_WAIT_NOTIFY)
        {
            s_status.state = OTA_STATE_CHECKING;
            snprintf(s_status.msg, sizeof(s_status.msg), "checking...");
            ota_http_phase();
        }

        /* ---- MQTT 阶段：订阅 inform 通知，等待平台推送 ---- */
        if (s_status.state == OTA_STATE_WAIT_NOTIFY)
        {
            mqtt_cfg_t cfg;
            uint32_t last_check = HAL_GetTick();
            int recheck = 0;
            int mqtt_ret;

            memset(&cfg, 0, sizeof(cfg));
            snprintf(cfg.host, sizeof(cfg.host), "%s", OTA_MQTT_HOST);
            cfg.port = OTA_MQTT_PORT;
            /* OneNET 要求 clientId = 设备名称（不能加前缀） */
            snprintf(cfg.client_id, sizeof(cfg.client_id), "%s", OTA_DEVICE_ID);
            snprintf(cfg.username, sizeof(cfg.username), "%s", OTA_MQTT_USER);
            snprintf(cfg.password, sizeof(cfg.password), "%s", OTA_MQTT_PASS);
            cfg.keepalive = OTA_MQTT_KEEPALIVE;
            mqtt_init(&cfg);

            /* [FIX-10] 区分鉴权失败与网络失败：
               MQTT_AUTH_ERR = CONNACK 拒绝（clientId/用户名/token 错，
               OneNET token 过期等），属配置问题，重试无意义也按退避走，
               但串口信息直接给出判断方向 */
            mqtt_ret = mqtt_connect(8000);
            if (mqtt_ret != MQTT_OK)
            {
                if (mqtt_ret == MQTT_AUTH_ERR)
                {
                    snprintf(s_status.msg, sizeof(s_status.msg), "mqtt auth fail (check token/clientId)");
                }
                else
                {
                    snprintf(s_status.msg, sizeof(s_status.msg), "mqtt conn fail");
                }
                s_status.state = OTA_STATE_ERROR;
                mqtt_disconnect();   /* [FIX-9] 收尾清理，避免模块残留中间态 */
                ota_fail_wait();
                continue;
            }
            ota_success_reset();
            if (mqtt_subscribe(OTA_TOPIC_INFORM, 0, 3000) != MQTT_OK)
            {
                snprintf(s_status.msg, sizeof(s_status.msg), "subscribe fail");
                s_status.state = OTA_STATE_ERROR;
                mqtt_disconnect();
                ota_fail_wait();
                continue;
            }
            ota_success_reset();
            snprintf(s_status.msg, sizeof(s_status.msg), "waiting notify...");
            last_ping = HAL_GetTick();

            /* 等待 inform 通知 / 周期轮询 / 断线 */
            while (s_status.state == OTA_STATE_WAIT_NOTIFY)
            {
                int r;

                ota_feed_watchdog();

                r = mqtt_loop(1000, on_mqtt_publish, NULL);
                if (r == MQTT_DISCONNECTED)
                {
                    snprintf(s_status.msg, sizeof(s_status.msg), "mqtt lost, reconnect");
                    s_status.state = OTA_STATE_ERROR;
                    break;
                }

                /* 收到平台 OTA 通知 / UI 手动触发：退出等待，重新执行 HTTP 检测 */
                if (s_inform_received)
                {
                    s_inform_received = 0;
                    recheck = 1;
                    break;
                }
                if (s_check_requested)
                {
                    s_check_requested = 0;
                    recheck = 1;
                    break;
                }

                /* 心跳：keepalive/2 */
                if ((HAL_GetTick() - last_ping) >= (uint32_t)OTA_MQTT_KEEPALIVE * 500U)
                {
                    mqtt_ping(2000);
                    last_ping = HAL_GetTick();
                }

                /* 周期轮询兜底（OTA_CHECK_PERIOD_MS=0 时禁用） */
                if (OTA_CHECK_PERIOD_MS > 0 &&
                    (HAL_GetTick() - last_check) >= OTA_CHECK_PERIOD_MS)
                {
                    recheck = 1;
                    break;
                }
            }

            mqtt_disconnect();
            if (recheck)
            {
                s_status.state = OTA_STATE_CHECKING;   /* 下一轮直接进入 HTTP 检测 */
            }
        }
    }
}
