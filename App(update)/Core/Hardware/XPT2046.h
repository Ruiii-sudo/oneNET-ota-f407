#ifndef __XPT2046_H
#define __XPT2046_H

#include "main.h"
#include "gpio.h"
#include "LCD.h"
#include <stdint.h>

/* XPT2046 引脚定义（MSP2807接线）
 * T_CLK  -> PC10
 * T_CS   -> PC13
 * T_DIN  -> PC12
 * T_DO   -> PC11  (输入上拉)
 * T_IRQ  -> PC8   (本工程使用轮询，未使用中断)
 */
#define XPT2046_CLK_PORT    GPIOC
#define XPT2046_CLK_PIN     GPIO_PIN_10
#define XPT2046_CS_PORT     GPIOC
#define XPT2046_CS_PIN      GPIO_PIN_13
#define XPT2046_DIN_PORT    GPIOC
#define XPT2046_DIN_PIN     GPIO_PIN_12
#define XPT2046_DO_PORT     GPIOC
#define XPT2046_DO_PIN      GPIO_PIN_11

/* 引脚操作宏 */
#define XPT2046_CLK_HIGH()  HAL_GPIO_WritePin(XPT2046_CLK_PORT, XPT2046_CLK_PIN, GPIO_PIN_SET)
#define XPT2046_CLK_LOW()   HAL_GPIO_WritePin(XPT2046_CLK_PORT, XPT2046_CLK_PIN, GPIO_PIN_RESET)
#define XPT2046_CS_HIGH()   HAL_GPIO_WritePin(XPT2046_CS_PORT, XPT2046_CS_PIN, GPIO_PIN_SET)
#define XPT2046_CS_LOW()    HAL_GPIO_WritePin(XPT2046_CS_PORT, XPT2046_CS_PIN, GPIO_PIN_RESET)
#define XPT2046_DIN_HIGH()  HAL_GPIO_WritePin(XPT2046_DIN_PORT, XPT2046_DIN_PIN, GPIO_PIN_SET)
#define XPT2046_DIN_LOW()   HAL_GPIO_WritePin(XPT2046_DIN_PORT, XPT2046_DIN_PIN, GPIO_PIN_RESET)
#define XPT2046_DO_READ()   HAL_GPIO_ReadPin(XPT2046_DO_PORT, XPT2046_DO_PIN)

/* T_IRQ 引脚（PC8）：未按下时高电平，按下时拉低 */
#define XPT2046_IRQ_PORT    GPIOC
#define XPT2046_IRQ_PIN     GPIO_PIN_8
#define XPT2046_IRQ_READ()  HAL_GPIO_ReadPin(XPT2046_IRQ_PORT, XPT2046_IRQ_PIN)
#define XPT2046_IS_PRESSED()  (XPT2046_IRQ_READ() == GPIO_PIN_RESET)  /* 低电平=按下 */

/* XPT2046 命令字 */
#define XPT2046_CMD_X       0x90    /* 读X坐标，12bit，差分模式 */
#define XPT2046_CMD_Y       0xD0    /* 读Y坐标，12bit，差分模式 */
#define XPT2046_CMD_Z1      0xB0    /* 读Z1（触摸压力） */
#define XPT2046_CMD_Z2      0xC0    /* 读Z2（触摸压力） */

/* 触摸压力阈值，小于此值认为未按下 */
#define XPT2046_PRESS_THRESHOLD  300

/* 坐标校准参数
 * 原始AD值范围约 0~4095，需要映射到屏幕坐标
 * 这些参数需要根据实际屏幕方向调整
 * formula: screen_x = (raw_x - XPT_X_MIN) * LCD_W / (XPT_X_MAX - XPT_X_MIN)
 *          screen_y = (raw_y - XPT_Y_MIN) * LCD_H / (XPT_Y_MAX - XPT_Y_MIN)
 */
/* 根据MSP2807实际校准数据（五角落法，2026-08-26）
 * raw_x: 顶部≈3870, 底部≈440（方向反转）
 * raw_y: 左边≈300, 右边≈3780
 * 中心点: raw_x≈2206, raw_y≈2100（线性度良好）
 */
#define XPT_X_MIN   440     /* raw_x 最小值（屏幕底部） */
#define XPT_X_MAX   3870    /* raw_x 最大值（屏幕顶部） */
#define XPT_Y_MIN   300     /* raw_y 最小值（屏幕左边） */
#define XPT_Y_MAX   3780    /* raw_y 最大值（屏幕右边） */

/* 函数声明 */
uint8_t  XPT2046_Init(void);
uint16_t XPT2046_Read_AD(uint8_t cmd);
uint8_t  XPT2046_Read_XY(uint16_t *x, uint16_t *y);
uint8_t  XPT2046_IsPressed(void);
uint8_t  XPT2046_Scan(void);

/* 调试用全局变量（可通过串口查看原始AD值） */
extern volatile uint16_t g_xpt2046_raw_x;
extern volatile uint16_t g_xpt2046_raw_y;
extern volatile uint8_t  g_xpt2046_pressed;

#endif /* __XPT2046_H */
