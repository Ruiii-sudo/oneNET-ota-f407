/**
 ******************************************************************************
 * @file    ota_ui.h
 * @brief   OTA 界面（LVGL 悬浮面板：提示/进度/确认/结果）
 ******************************************************************************
 */
#ifndef __OTA_UI_H
#define __OTA_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/* 在 LVGL 初始化完成、主界面创建后调用（LVGL 任务上下文） */
void ota_ui_init(void);

/* 由 LVGL 任务周期性调用（建议 100-200ms），刷新状态显示 */
void ota_ui_refresh(void);

/* 悬浮面板可见性控制（OTA 任务可调用） */
void ota_ui_show(void);
void ota_ui_hide(void);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_UI_H */
