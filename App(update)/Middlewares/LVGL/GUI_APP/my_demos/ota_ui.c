/**
 ******************************************************************************
 * @file    ota_ui.c
 * @brief   OTA 界面 v2.0（LVGL 深色科技风仪表盘，240x320 竖屏）
 *
 * 设计说明：
 *  - 保留原 API：ota_ui_init / ota_ui_refresh / ota_ui_show / ota_ui_hide
 *  - 主界面：顶部状态栏 + 环形进度（LV_USE_ARC=1 时）/ 圆形状态标（=0 时）
 *    + 状态标题 + 副标题（msg）+ 进度条 + 底部双按钮（CHECK/ABOUT）
 *  - 信息页：ABOUT 按钮切入，显示设备 ID / 固件版本 / 活动槽位 / 备份状态 /
 *    启动标志 / 启动计数，数据来自参数区 param_area_load()
 *  - 文案语言：OTA_UI_LANG_ZH=1 使用中文（需 lv_conf.h 打开
 *    LV_FONT_SIMSUN_16_CJK），默认 0 使用英文（直接可编译）
 *
 * 修复记录：
 *  - 进度条常驻显示：不再按状态隐藏，待机为 0% 灰条，下载/校验/重启时填充。
 *    原因：设备连不上网时状态停在 CONNECTING/ERROR，永远不会触发下载态，
 *    旧逻辑下进度条永远不会出现。
 *  - ARC 分支增加圆盘底衬、轨道提亮，保证环在深背景上可见。
 *
 * 线程说明：LVGL 对象仅在 LVGL 任务上下文创建与刷新；OTA 任务只写
 * ota_status_t，本模块通过 ota_get_status() 只读快照。
 ******************************************************************************
 */
#include "ota_ui.h"
#include "ota.h"
#include "ota_config.h"
#include "param_area.h"
#include "ota_params.h"
#include "partition.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>

/* ================= 文案语言 ================= */
#ifndef OTA_UI_LANG_ZH
#define OTA_UI_LANG_ZH 0      /* 1=中文（需 LV_FONT_SIMSUN_16_CJK=1） */
#endif

#if OTA_UI_LANG_ZH
  #define TXT_BRAND      "OTA"
  #define TXT_DEVINFO    "设备信息"
  #define TXT_BACK       "返回"
  #define TXT_CHECK      "检查更新"
  #define TXT_ABOUT      "设备信息"
  #define TXT_IDLE       "待机"
  #define TXT_CONNECT    "连接中..."
  #define TXT_WAIT       "等待升级指令"
  #define TXT_CHECKING   "检测中"
  #define TXT_DOWNLOAD   "下载中"
  #define TXT_VERIFY     "校验中"
  #define TXT_REBOOT     "即将重启"
  #define TXT_ERROR      "错误"
  #define TXT_FW         "固件"
  #define TXT_SLOT       "活动分区"
  #define TXT_BACKUP     "备份"
  #define TXT_FLAG       "启动标志"
  #define TXT_COUNT      "启动计数"
  #define TXT_DEV_ID     "设备ID"
#else
  #define TXT_BRAND      "OTA"
  #define TXT_DEVINFO    "DEVICE INFO"
  #define TXT_BACK       "< BACK"
  #define TXT_CHECK      "CHECK"
  #define TXT_ABOUT      "ABOUT"
  #define TXT_IDLE       "STANDBY"
  #define TXT_CONNECT    "CONNECTING..."
  #define TXT_WAIT       "WAITING CMD"
  #define TXT_CHECKING   "CHECKING"
  #define TXT_DOWNLOAD   "DOWNLOADING"
  #define TXT_VERIFY     "VERIFYING"
  #define TXT_REBOOT     "REBOOTING"
  #define TXT_ERROR      "ERROR"
  #define TXT_FW         "Firmware"
  #define TXT_SLOT       "Active Slot"
  #define TXT_BACKUP     "Backup"
  #define TXT_FLAG       "Boot Flag"
  #define TXT_COUNT      "Boot Count"
  #define TXT_DEV_ID     "Device ID"
#endif

/* ================= 字体 ================= */
/* 中文模式依赖 lv_conf.h 打开 LV_FONT_SIMSUN_16_CJK；否则回退英文 Montserrat */
#if OTA_UI_LANG_ZH && LV_FONT_SIMSUN_16_CJK
  #define F_TITLE    &lv_font_simsun_16_cjk
  #define F_BODY     &lv_font_simsun_16_cjk
  #define F_SMALL    &lv_font_simsun_16_cjk
#else
  #define F_TITLE    &lv_font_montserrat_20
  #define F_BODY     &lv_font_montserrat_14
  #define F_SMALL    &lv_font_montserrat_12
#endif

/* ================= 配色 ================= */
#define C_BG        0x0F172A   /* 背景：深藏蓝 */
#define C_CARD      0x1E293B   /* 卡片/状态栏 */
#define C_ACCENT    0x38BDF8   /* 主色：天空蓝 */
#define C_ACCENT_DK 0x0EA5E9   /* 按钮主色 */
#define C_OK        0x4ADE80   /* 成功绿 */
#define C_WARN      0xFACC15   /* 警告黄 */
#define C_ERR       0xF87171   /* 错误红 */
#define C_TXT       0xF1F5F9   /* 主文字 */
#define C_TXT_DIM   0x94A3B8   /* 次文字 */
#define C_TRACK     0x334155   /* 进度条轨道/按钮底色 */

/* 各状态对应强调色：IDLE/CONNECT/WAIT/CHECK/DL/VERIFY/REBOOT/ERR */
static const uint32_t s_state_color[] = {
    C_TXT_DIM, C_WARN, C_ACCENT, C_WARN, C_ACCENT, 0xC084FC, C_OK, C_ERR
};

/* 各状态标题（与 ota_state_t 顺序一致） */
static const char *const s_state_txt[] = {
    TXT_IDLE, TXT_CONNECT, TXT_WAIT, TXT_CHECKING, TXT_DOWNLOAD, TXT_VERIFY, TXT_REBOOT, TXT_ERROR
};

/* ================= 对象句柄 ================= */
static lv_obj_t *s_ring;       /* 环形进度（ARC）或圆形状态标（obj） */
static lv_obj_t *s_ring_val;   /* 环中心文本 */
static lv_obj_t *s_title;      /* 状态主标题 */
static lv_obj_t *s_sub;        /* 副标题（msg/字节） */
static lv_obj_t *s_bar;        /* 进度条（常驻显示，百分比显示在环中心） */
static lv_obj_t *s_msg;        /* 底部消息 */
static lv_obj_t *s_page_about; /* 信息页 */
static lv_obj_t *s_about_val[6];
static uint32_t  s_last_state = 99;

/* ================= 工具函数 ================= */

static void fmt_kb(uint32_t n, char *buf, uint32_t sz)
{
    if (n >= (1024U * 1024U))
    {
        snprintf(buf, sz, "%lu.%lu MB",
                 (unsigned long)(n >> 20),
                 (unsigned long)(((n >> 10) & 1023U) * 10U / 1024U));
    }
    else
    {
        snprintf(buf, sz, "%lu KB", (unsigned long)((n + 1023U) >> 10));
    }
}

static void fmt_ver(uint32_t v, char *buf, uint32_t sz)
{
    snprintf(buf, sz, "v%lu.%lu.%lu",
             (unsigned long)((v >> 16) & 0xFFU),
             (unsigned long)((v >> 8) & 0xFFU),
             (unsigned long)(v & 0xFFU));
}

/* ================= 事件回调 ================= */

static void ota_ui_btn_check_cb(lv_event_t *e)
{
    (void)e;
    ota_request_check();
}

static void ota_ui_btn_about_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_clear_flag(s_page_about, LV_OBJ_FLAG_HIDDEN);
}

static void ota_ui_btn_back_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_page_about, LV_OBJ_FLAG_HIDDEN);
}

static void ota_ui_btn_rollback_cb(lv_event_t *e)
{
    (void)e;
    ota_manual_rollback();
}

static void ota_ui_btn_recovery_cb(lv_event_t *e)
{
    (void)e;
    ota_manual_recovery();
}

/* ================= 对象创建 ================= */

static void ota_ui_build_main(lv_obj_t *scr)
{
    /* ---- 顶部状态栏 ---- */
    lv_obj_t *top = lv_obj_create(scr);
    lv_obj_set_size(top, 240, 40);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(top, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(top, 0, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);

    lv_obj_t *l_brand = lv_label_create(top);
    lv_label_set_text(l_brand, TXT_BRAND);
    lv_obj_set_style_text_color(l_brand, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_text_font(l_brand, F_BODY, 0);
    lv_obj_align(l_brand, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *l_info = lv_label_create(top);
    lv_label_set_text(l_info, "STM32F407");
    lv_obj_set_style_text_color(l_info, lv_color_hex(C_TXT_DIM), 0);
    lv_obj_set_style_text_font(l_info, F_SMALL, 0);
    lv_obj_align(l_info, LV_ALIGN_RIGHT_MID, -12, 0);

    /* ---- 环形进度 / 圆形状态标 ---- */
#if LV_USE_ARC
    /* 环形底衬：深色圆盘，衬托弧线（否则轨道在深背景上几乎不可见） */
    lv_obj_t *ring_bg = lv_obj_create(scr);
    lv_obj_set_size(ring_bg, 118, 118);
    lv_obj_align(ring_bg, LV_ALIGN_CENTER, 0, -48);
    lv_obj_set_style_bg_color(ring_bg, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(ring_bg, 59, 0);
    lv_obj_set_style_border_width(ring_bg, 0, 0);
    lv_obj_set_style_shadow_width(ring_bg, 0, 0);

    s_ring = lv_arc_create(scr);
    lv_obj_set_size(s_ring, 104, 104);
    lv_obj_align(s_ring, LV_ALIGN_CENTER, 0, -48);
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_TRANSP, 0);
    lv_arc_set_rotation(s_ring, 270);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_range(s_ring, 0, 100);
    lv_arc_set_value(s_ring, 0);
    lv_obj_remove_style(s_ring, NULL, LV_PART_KNOB);   /* 去掉旋钮 */
    lv_obj_remove_style(s_ring, NULL, LV_PART_MAIN);   /* 去默认描边 */
    lv_obj_set_style_arc_width(s_ring, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(0x475569), LV_PART_MAIN); /* 提亮轨道 */
    lv_obj_set_style_arc_width(s_ring, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_ring, 0, LV_PART_INDICATOR);
#else
    s_ring = lv_obj_create(scr);
    lv_obj_set_size(s_ring, 104, 104);
    lv_obj_align(s_ring, LV_ALIGN_CENTER, 0, -48);
    lv_obj_set_style_bg_color(s_ring, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(s_ring, 52, 0);
    lv_obj_set_style_border_width(s_ring, 3, 0);
    lv_obj_set_style_border_color(s_ring, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_border_opa(s_ring, LV_OPA_COVER, 0);
#endif

    s_ring_val = lv_label_create(s_ring);
    lv_label_set_text(s_ring_val, "--");
    lv_obj_set_style_text_color(s_ring_val, lv_color_hex(C_TXT), 0);
    lv_obj_set_style_text_font(s_ring_val, F_TITLE, 0);
    lv_obj_center(s_ring_val);

    /* ---- 状态标题 ---- */
    s_title = lv_label_create(scr);
    lv_label_set_text(s_title, s_state_txt[OTA_STATE_IDLE]);
    lv_obj_set_style_text_color(s_title, lv_color_hex(C_TXT_DIM), 0);
    lv_obj_set_style_text_font(s_title, F_TITLE, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 172);

    /* ---- 副标题（msg） ---- */
    s_sub = lv_label_create(scr);
    lv_label_set_text(s_sub, "");
    lv_obj_set_width(s_sub, 224);
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(C_TXT_DIM), 0);
    lv_obj_set_style_text_font(s_sub, F_BODY, 0);
    lv_obj_align(s_sub, LV_ALIGN_TOP_MID, 50, 200);   /* 用户手调：x=50 */

    /* ---- 进度条（常驻显示，待机 0% 灰条；百分比由环中心承担） ---- */
    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, 168, 14);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 230);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);

    /* ---- 底部消息 ---- */
    s_msg = lv_label_create(scr);
    lv_label_set_text(s_msg, "");
    lv_obj_set_width(s_msg, 224);
    lv_label_set_long_mode(s_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_msg, lv_color_hex(C_TXT_DIM), 0);
    lv_obj_set_style_text_font(s_msg, F_SMALL, 0);
    lv_obj_align(s_msg, LV_ALIGN_TOP_MID, 40, 254);   /* 用户手调：x=40 */

    /* ---- 底部按钮 ---- */
    lv_obj_t *btn_check = lv_btn_create(scr);
    lv_obj_set_size(btn_check, 104, 40);
    lv_obj_align(btn_check, LV_ALIGN_BOTTOM_LEFT, 12, -8);
    lv_obj_set_style_bg_color(btn_check, lv_color_hex(C_ACCENT_DK), 0);
    lv_obj_set_style_radius(btn_check, 8, 0);
    lv_obj_set_style_shadow_width(btn_check, 0, 0);
    lv_obj_t *l_check = lv_label_create(btn_check);
    lv_label_set_text(l_check, TXT_CHECK);
    lv_obj_set_style_text_color(l_check, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l_check, F_BODY, 0);
    lv_obj_center(l_check);
    lv_obj_add_event_cb(btn_check, ota_ui_btn_check_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_about = lv_btn_create(scr);
    lv_obj_set_size(btn_about, 104, 40);
    lv_obj_align(btn_about, LV_ALIGN_BOTTOM_RIGHT, -12, -8);
    lv_obj_set_style_bg_color(btn_about, lv_color_hex(C_TRACK), 0);
    lv_obj_set_style_radius(btn_about, 8, 0);
    lv_obj_set_style_shadow_width(btn_about, 0, 0);
    lv_obj_t *l_about = lv_label_create(btn_about);
    lv_label_set_text(l_about, TXT_ABOUT);
    lv_obj_set_style_text_color(l_about, lv_color_hex(C_TXT), 0);
    lv_obj_set_style_text_font(l_about, F_BODY, 0);
    lv_obj_center(l_about);
    lv_obj_add_event_cb(btn_about, ota_ui_btn_about_cb, LV_EVENT_CLICKED, NULL);
}

static void ota_ui_build_about(lv_obj_t *scr)
{
    const char *keys[6] = { TXT_DEV_ID, TXT_FW, TXT_SLOT, TXT_BACKUP, TXT_FLAG, TXT_COUNT };
    int i;

    s_page_about = lv_obj_create(scr);
    lv_obj_set_size(s_page_about, 240, 320);
    lv_obj_align(s_page_about, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_page_about, lv_color_hex(C_BG), 0);
    lv_obj_set_style_radius(s_page_about, 0, 0);
    lv_obj_set_style_border_width(s_page_about, 0, 0);
    lv_obj_set_style_pad_all(s_page_about, 0, 0);
    lv_obj_add_flag(s_page_about, LV_OBJ_FLAG_HIDDEN);

    /* 顶部栏 */
    lv_obj_t *top = lv_obj_create(s_page_about);
    lv_obj_set_size(top, 240, 40);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(top, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(top, 0, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);

    lv_obj_t *l_back = lv_label_create(top);
    lv_label_set_text(l_back, TXT_BACK);
    lv_obj_set_style_text_color(l_back, lv_color_hex(C_TXT), 0);
    lv_obj_set_style_text_font(l_back, F_BODY, 0);
    lv_obj_align(l_back, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *l_title = lv_label_create(top);
    lv_label_set_text(l_title, TXT_DEVINFO);
    lv_obj_set_style_text_color(l_title, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_text_font(l_title, F_BODY, 0);
    lv_obj_align(l_title, LV_ALIGN_RIGHT_MID, -12, 0);

    /* 让"返回"可点击 */
    lv_obj_t *btn_back = lv_btn_create(top);
    lv_obj_set_size(btn_back, 80, 36);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_set_style_shadow_width(btn_back, 0, 0);
    lv_obj_add_event_cb(btn_back, ota_ui_btn_back_cb, LV_EVENT_CLICKED, NULL);

    /* 信息卡片 */
    lv_obj_t *card = lv_obj_create(s_page_about);
    lv_obj_set_size(card, 216, 200);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 12, 0);

    for (i = 0; i < 6; i++)
    {
        lv_obj_t *row = lv_obj_create(card);
        lv_obj_set_size(row, 192, 26);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, i * 29);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, (i < 5) ? 1 : 0, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(C_TXT_DIM), 0);
        lv_obj_set_style_border_opa(row, LV_OPA_30, 0);
        lv_obj_set_style_pad_all(row, 0, 0);

        lv_obj_t *k = lv_label_create(row);
        lv_label_set_text(k, keys[i]);
        lv_obj_set_style_text_color(k, lv_color_hex(C_TXT_DIM), 0);
        lv_obj_set_style_text_font(k, F_SMALL, 0);
        lv_obj_align(k, LV_ALIGN_LEFT_MID, 4, 0);

        s_about_val[i] = lv_label_create(row);
        lv_label_set_text(s_about_val[i], "--");
        lv_obj_set_style_text_color(s_about_val[i], lv_color_hex(C_TXT), 0);
        lv_obj_set_style_text_font(s_about_val[i], F_SMALL, 0);
        lv_obj_align(s_about_val[i], LV_ALIGN_RIGHT_MID, -4, 0);
    }

    /* 回滚按钮 */
    lv_obj_t *btn_rollback = lv_btn_create(s_page_about);
    lv_obj_set_size(btn_rollback, 100, 32);
    lv_obj_align(btn_rollback, LV_ALIGN_TOP_LEFT, 16, 272);
    lv_obj_set_style_bg_color(btn_rollback, lv_color_hex(0x9A6A2F), 0);
    lv_obj_set_style_radius(btn_rollback, 6, 0);
    lv_obj_set_style_border_width(btn_rollback, 0, 0);
    lv_obj_t *lbl_rb = lv_label_create(btn_rollback);
    lv_label_set_text(lbl_rb, "Rollback");
    lv_obj_set_style_text_color(lbl_rb, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_rb, F_SMALL, 0);
    lv_obj_center(lbl_rb);
    lv_obj_add_event_cb(btn_rollback, ota_ui_btn_rollback_cb, LV_EVENT_CLICKED, NULL);

    /* 恢复出厂按钮 */
    lv_obj_t *btn_recovery = lv_btn_create(s_page_about);
    lv_obj_set_size(btn_recovery, 100, 32);
    lv_obj_align(btn_recovery, LV_ALIGN_TOP_RIGHT, -16, 272);
    lv_obj_set_style_bg_color(btn_recovery, lv_color_hex(0xA14E50), 0);
    lv_obj_set_style_radius(btn_recovery, 6, 0);
    lv_obj_set_style_border_width(btn_recovery, 0, 0);
    lv_obj_t *lbl_rec = lv_label_create(btn_recovery);
    lv_label_set_text(lbl_rec, "Factory Reset");
    lv_obj_set_style_text_color(lbl_rec, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_rec, F_SMALL, 0);
    lv_obj_center(lbl_rec);
    lv_obj_add_event_cb(btn_recovery, ota_ui_btn_recovery_cb, LV_EVENT_CLICKED, NULL);
}

void ota_ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    ota_param_t p;
    char buf[48];

    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);

    ota_ui_build_main(scr);
    ota_ui_build_about(scr);

    lv_label_set_text(s_msg, "System OK, OTA enabled");

    /* ---- 信息页：从参数区读取并填充（启动时一次） ---- */
    if (param_area_load(&p) == 0)
    {
        lv_label_set_text(s_about_val[0], OTA_DEVICE_ID);

        fmt_ver(p.version, buf, sizeof(buf));
        lv_label_set_text(s_about_val[1], buf);

        snprintf(buf, sizeof(buf), "A @0x%08lX", (unsigned long)OTA_APP_A_ADDR);
        lv_label_set_text(s_about_val[2], buf);

        lv_label_set_text(s_about_val[3],
                          (p.backup_status == OTA_BACKUP_VALID) ? "VALID" :
                          (p.backup_status == OTA_BACKUP_DOWNLOADING) ? "DL..." : "EMPTY");

        lv_label_set_text(s_about_val[4],
                          (p.boot_flag == OTA_BOOT_FLAG_TRY_NEW) ? "TRY_NEW" : "NORMAL");

        snprintf(buf, sizeof(buf), "%lu", (unsigned long)p.boot_count);
        lv_label_set_text(s_about_val[5], buf);
    }
}

/* ================= 刷新 ================= */

void ota_ui_refresh(void)
{
    ota_status_t st;
    char buf[48];

    if (s_title == NULL)
    {
        return;
    }

    ota_get_status(&st);

    /* 状态切换：更新标题、颜色（进度条常驻，不再按状态隐藏） */
    if (st.state != s_last_state)
    {
        uint32_t c;

        s_last_state = st.state;

        lv_label_set_text(s_title, s_state_txt[st.state]);

        c = (st.state <= OTA_STATE_ERROR) ? s_state_color[st.state] : C_TXT_DIM;
        lv_obj_set_style_text_color(s_title, lv_color_hex(c), 0);
        lv_obj_set_style_border_color(s_ring, lv_color_hex(c), 0);
#if LV_USE_ARC
        lv_obj_set_style_arc_color(s_ring, lv_color_hex(c), LV_PART_INDICATOR);
#else
        lv_obj_set_style_border_opa(s_ring, LV_OPA_COVER, 0);
#endif
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(c), LV_PART_INDICATOR);
    }

    /* 环形/圆心中间内容：下载类显示百分比，否则显示状态符号 */
    switch (st.state)
    {
    case OTA_STATE_DOWNLOADING:
    case OTA_STATE_VERIFYING:
    case OTA_STATE_CONFIRM_REBOOT:
        snprintf(buf, sizeof(buf), "%lu%%", (unsigned long)st.progress);
        lv_label_set_text(s_ring_val, buf);
        break;
    case OTA_STATE_IDLE:
        lv_label_set_text(s_ring_val, "OK");
        break;
    case OTA_STATE_CONNECTING:
    case OTA_STATE_WAIT_NOTIFY:
    case OTA_STATE_CHECKING:
        lv_label_set_text(s_ring_val, "...");
        break;
    case OTA_STATE_ERROR:
        lv_label_set_text(s_ring_val, "!");
        break;
    default:
        break;
    }

    /* 进度条（常驻更新，待机时为 0；百分比显示在环中心） */
    lv_bar_set_value(s_bar, (int32_t)st.progress, LV_ANIM_ON);

    /* 副标题：下载类显示"已下载/总字节"，其余显示 msg */
    if (st.state == OTA_STATE_DOWNLOADING && st.total > 0)
    {
        char a[16], b[16];
        fmt_kb(st.downloaded, a, sizeof(a));
        fmt_kb(st.total, b, sizeof(b));
        snprintf(buf, sizeof(buf), "%s / %s", a, b);
        lv_label_set_text(s_sub, buf);
    }
    else
    {
        lv_label_set_text(s_sub, st.msg);
    }

    /* 底部消息：错误时红色，重启时绿色 */
    if (st.state == OTA_STATE_ERROR)
    {
        lv_obj_set_style_text_color(s_msg, lv_color_hex(C_ERR), 0);
        lv_label_set_text(s_msg, st.msg);
    }
    else if (st.state == OTA_STATE_CONFIRM_REBOOT)
    {
        lv_obj_set_style_text_color(s_msg, lv_color_hex(C_OK), 0);
        lv_label_set_text(s_msg, st.msg);
    }
	
	{
        ota_param_t ap;
        static uint32_t s_prev_ver  = 0xFFFFFFFF;
        static uint8_t  s_prev_bk   = 0xFF;
        static uint8_t  s_prev_flag  = 0xFF;
        static uint32_t s_prev_cnt   = 0xFFFFFFFF;

        if (param_area_load(&ap) == 0)
        {
            /* 版本号 */
            if (ap.version != s_prev_ver)
            {
                char vbuf[16];
                fmt_ver(ap.version, vbuf, sizeof(vbuf));
                lv_label_set_text(s_about_val[1], vbuf);
                s_prev_ver = ap.version;
            }

            /* 槽位（固定 A 槽，不用刷新） */

            /* 备份状态 */
            if (ap.backup_status != s_prev_bk)
            {
                lv_label_set_text(s_about_val[3],
                                  (ap.backup_status == OTA_BACKUP_VALID) ? "VALID" :
                                  (ap.backup_status == OTA_BACKUP_DOWNLOADING) ? "DL..." : "EMPTY");
                s_prev_bk = ap.backup_status;
            }

            /* 启动标志 */
            if (ap.boot_flag != s_prev_flag)
            {
                lv_label_set_text(s_about_val[4],
                                  (ap.boot_flag == OTA_BOOT_FLAG_TRY_NEW) ? "TRY_NEW" : "NORMAL");
                s_prev_flag = ap.boot_flag;
            }

            /* 启动计数 */
            if (ap.boot_count != s_prev_cnt)
            {
                char cbuf[16];
                snprintf(cbuf, sizeof(cbuf), "%lu", (unsigned long)ap.boot_count);
                lv_label_set_text(s_about_val[5], cbuf);
                s_prev_cnt = ap.boot_count;
            }
        }
    }
}

/* ================= 显隐控制 ================= */

void ota_ui_show(void)
{
    lv_obj_clear_flag(s_page_about, LV_OBJ_FLAG_HIDDEN); /* 由 ABOUT 控制 */
}

void ota_ui_hide(void)
{
    lv_obj_add_flag(s_page_about, LV_OBJ_FLAG_HIDDEN);
}
