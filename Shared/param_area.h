/**
 ******************************************************************************
 * @file    param_area.h
 * @brief   参数区管理（双槽冗余 + CRC32）
 ******************************************************************************
 */
#ifndef __PARAM_AREA_H
#define __PARAM_AREA_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ota_params.h"

/*
 * 从参数区加载参数。
 * 返回 0：成功；-1：双槽均无效（调用者应调用 param_area_defaults + param_area_save 重建）
 */
int param_area_load(ota_param_t *param);

/*
 * 保存参数到参数区（双槽写入）。
 * 返回 0：成功；-1：失败
 */
int param_area_save(const ota_param_t *param);

/* 用默认值填充参数结构体（不写 Flash） */
void param_area_defaults(ota_param_t *param);

/* 计算结构体 CRC（不含 crc32 字段） */
uint32_t param_area_crc(const ota_param_t *param);

#ifdef __cplusplus
}
#endif

#endif /* __PARAM_AREA_H */
