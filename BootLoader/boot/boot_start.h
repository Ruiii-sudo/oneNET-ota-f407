#ifndef BOOT_START_H
#define BOOT_START_H

#include "stm32f4xx_hal.h"

/**
 * @brief  BootLoader 完整业务流程入口
 *         加载参数区 → 启动标志处理(TRY_NEW/计数/回滚) →
 *         校验 App 向量表 → 启动独立看门狗 → 跳转活动分区
 * @note   该函数不会返回（最终跳转到 App）
 */
void boot_run(void);

#endif /* BOOT_LOGIC_H */
