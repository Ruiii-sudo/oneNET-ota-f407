/**
 ******************************************************************************
 * @file    app_jump.c
 * @brief   从 Bootloader 跳转到 App 分区
 *
 * 跳转要点：
 *  1. 关闭全局中断（PRIMASK）
 *  2. 停掉 SysTick 与系统时钟外设（可选，避免外设残留状态）
 *  3. 从 App 向量表读取初始 MSP 并设置
 *  4. 重定位向量表 SCB->VTOR = app_addr
 *  5. 读取复位向量并跳转
 ******************************************************************************
 */
#include "app_jump.h"
#include "partition.h"
#include "main.h"
#include "usart.h"

#define APP_STACK_RAM_BASE   0x20000000UL
#define APP_STACK_RAM_END    0x20030000UL   /* F407VET6 192KB RAM 上界 */

int app_jump_validate(uint32_t app_addr)
{
    uint32_t msp, reset_vec;

    if (app_addr < OTA_APP_A_ADDR || app_addr >= OTA_APP_A_ADDR + OTA_APP_SIZE + 0x10000UL)
    {
        return -1;
    }

    /* 向量表前两字 */
    msp       = *(volatile uint32_t *)(app_addr);
    reset_vec = *(volatile uint32_t *)(app_addr + 4U);

    /* 初始栈指针须落在 RAM 内 */
    if (msp < APP_STACK_RAM_BASE || msp >= APP_STACK_RAM_END)
    {
        return -1;
    }

    /* 复位向量须落在 Flash 内（本芯片 512KB） */
    if (reset_vec < 0x08000000UL || reset_vec >= 0x08080000UL)
    {
        return -1;
    }

    return 0;
}

void app_jump_execute(uint32_t app_addr)
{
    uint32_t msp;
    uint32_t reset_vec;
    void (*jump)(void);
    int i;

    /* 1. 关闭全局中断 */
    __disable_irq();

    /* [FIX-14] 清空所有 NVIC 使能位与挂起位：
       BootLoader 侧若使能过任何外设中断（如 USART/DMA），跳转瞬间
       若恰有中断挂起，会在 App 的 HAL 初始化完成前触发，
       使用未初始化的句柄导致 HardFault。 */
    for (i = 0; i < 8; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }

    /* 2. 停止 SysTick */
    SysTick->CTRL = 0;

    /* 3. 读取并设置 MSP */
    msp = *(volatile uint32_t *)app_addr;
    __set_MSP(msp);

    /* 4. 重定位向量表（先写 VTOR 再使能中断，保证异常向量已切换） */
    SCB->VTOR = app_addr;
    __DSB();
    __ISB();
    __enable_irq();

    /* 5. 跳转 */
    reset_vec = *(volatile uint32_t *)(app_addr + 4U);
    jump = (void (*)(void))reset_vec;
    jump();

    /* 不应到达这里 */
    while (1)
    {
    }
}
