/**
 ******************************************************************************
 * @file    ota.h
 * @brief   OTA 升级状态机与任务（App 侧）
 ******************************************************************************
 */
#ifndef __OTA_H
#define __OTA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* OTA 状态（供 UI 显示） */
typedef enum {
    OTA_STATE_IDLE = 0,        /* 空闲/待机 */
    OTA_STATE_CONNECTING,      /* 连接 WiFi/Broker */
    OTA_STATE_WAIT_NOTIFY,     /* 等待服务器下发升级通知（MQTT inform） */
    OTA_STATE_CHECKING,        /* 检测升级任务（HTTP check） */
    OTA_STATE_CONFIRM_UPDATE,  /* 检测到更新，等待用户确认（UI 弹窗） */
    OTA_STATE_DOWNLOADING,     /* 下载固件 */
    OTA_STATE_VERIFYING,       /* MD5/SHA-256 校验 */
    OTA_STATE_TESTING,         /* 新固件静默测试中 */
    OTA_STATE_CONFIRM_REBOOT,  /* 升级完成，等待重启 */
    OTA_STATE_ERROR            /* 出错 */
} ota_state_t;

/* 供 UI 查询的状态快照 */
typedef struct {
    ota_state_t state;
    uint32_t    progress;      /* 0-100 */
    uint32_t    downloaded;    /* 已下载字节 */
    uint32_t    total;         /* 总字节 */
    char        msg[64];       /* 附加消息 */
} ota_status_t;


/* OTA 任务入口 */
void OTA_Task(void *pvParameters);

/* 手动请求立即检查一次升级（供 UI 按钮调用） */
void ota_request_check(void);

/* 更新确认（UI 弹窗按钮）：1=立即更新 0=暂不更新 */
void ota_confirm_update(int yes);

/* 手动回滚到上一版本（供 UI 按钮调用） */
void ota_manual_rollback(void);

/* 手动恢复出厂设置（供 UI 按钮调用） */
void ota_manual_recovery(void);

/* 读取当前状态快照 */
void ota_get_status(ota_status_t *st);

/* 喂独立看门狗（App 全周期维护） */
void ota_feed_watchdog(void);

/* 启动成功确认：新固件正常运行超窗口后调用，固化启动标志 */
void ota_confirm_boot_success(void);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_H */
