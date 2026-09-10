/**
 ******************************************************************************
 * @file    app_jump.h
 * @brief   从 Bootloader 跳转到 App 分区
 ******************************************************************************
 */
#ifndef __APP_JUMP_H
#define __APP_JUMP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*
 * 校验 App 分区是否有有效固件（向量表检查）：
 *  - 首字（初始 MSP）落在 RAM 范围 0x20000000-0x20030000
 *  - 次字（复位向量）落在 Flash 范围
 * 返回 0：有效；-1：无效
 */
int  app_jump_validate(uint32_t app_addr);

/* 跳转到指定地址的 App（不再返回） */
void app_jump_execute(uint32_t app_addr);

#ifdef __cplusplus
}
#endif

#endif /* __APP_JUMP_H */
