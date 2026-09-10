/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usart.c
  * @brief   This file provides code for the configuration
  *          of the USART instances.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "usart.h"

/* USER CODE BEGIN 0 */
/* [FIX-13] 串口打印线程安全�?
   USART1_Printf 使用共享静�?�缓�? + DMA，多任务（OTA_Task / LVGL_Task /
   HardFault 打印）并发调用会互相覆盖缓冲或误�? DMA 完成标志，导�?
   日志丢失/串行。引�? FreeRTOS 互斥锁串行化；ISR 上下文（HardFault�?
   不取锁，直接等待 DMA 完成�? */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

static SemaphoreHandle_t s_uart_mtx = NULL;

/* USER CODE END 0 */

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart3;
DMA_HandleTypeDef hdma_usart1_tx;
DMA_HandleTypeDef hdma_usart3_rx;

/* USART1 init function */

void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* 鎵嬪姩寮哄埗浣胯兘USART1 */
  USART1->CR1 |= (USART_CR1_UE | USART_CR1_TE | USART_CR1_RE);
  /* 绛夊�?1ms璁︰SART绋冲�? */
  HAL_Delay(1);

  /* USER CODE END USART1_Init 2 */

}
/* USART3 init function */

void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 57600;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

void HAL_UART_MspInit(UART_HandleTypeDef* uartHandle)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(uartHandle->Instance==USART1)
  {
  /* USER CODE BEGIN USART1_MspInit 0 */

  /* USER CODE END USART1_MspInit 0 */
    /* USART1 clock enable */
    __HAL_RCC_USART1_CLK_ENABLE();

    __HAL_RCC_GPIOA_CLK_ENABLE();
    /**USART1 GPIO Configuration
    PA9     ------> USART1_TX
    PA10     ------> USART1_RX
    */
    GPIO_InitStruct.Pin = GPIO_PIN_9|GPIO_PIN_10;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    /* USART1 DMA Init */
    /* USART1_TX Init */
    hdma_usart1_tx.Instance = DMA2_Stream7;
    hdma_usart1_tx.Init.Channel = DMA_CHANNEL_4;
    hdma_usart1_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
    hdma_usart1_tx.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_usart1_tx.Init.MemInc = DMA_MINC_ENABLE;
    hdma_usart1_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart1_tx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    hdma_usart1_tx.Init.Mode = DMA_NORMAL;
    hdma_usart1_tx.Init.Priority = DMA_PRIORITY_MEDIUM;
    hdma_usart1_tx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_usart1_tx) != HAL_OK)
    {
      Error_Handler();
    }

    __HAL_LINKDMA(uartHandle,hdmatx,hdma_usart1_tx);

    /* USART1 interrupt Init */
    HAL_NVIC_SetPriority(USART1_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
  /* USER CODE BEGIN USART1_MspInit 1 */

  /* USER CODE END USART1_MspInit 1 */
  }
  else if(uartHandle->Instance==USART3)
  {
  /* USER CODE BEGIN USART3_MspInit 0 */

  /* USER CODE END USART3_MspInit 0 */
    /* USART3 clock enable */
    __HAL_RCC_USART3_CLK_ENABLE();

    __HAL_RCC_GPIOB_CLK_ENABLE();
    /**USART3 GPIO Configuration
    PB10     ------> USART3_TX
    PB11     ------> USART3_RX
    */
    GPIO_InitStruct.Pin = GPIO_PIN_10|GPIO_PIN_11;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART3;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    /* USART3 DMA Init */
    /* USART3_RX Init */
    hdma_usart3_rx.Instance = DMA1_Stream1;
    hdma_usart3_rx.Init.Channel = DMA_CHANNEL_4;
    hdma_usart3_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hdma_usart3_rx.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_usart3_rx.Init.MemInc = DMA_MINC_ENABLE;
    hdma_usart3_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_usart3_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
    hdma_usart3_rx.Init.Mode = DMA_CIRCULAR;
    hdma_usart3_rx.Init.Priority = DMA_PRIORITY_VERY_HIGH;
    hdma_usart3_rx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_usart3_rx) != HAL_OK)
    {
      Error_Handler();
    }

    __HAL_LINKDMA(uartHandle,hdmarx,hdma_usart3_rx);

    /* USART3 interrupt Init */
    HAL_NVIC_SetPriority(USART3_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USART3_IRQn);
  /* USER CODE BEGIN USART3_MspInit 1 */

  /* USER CODE END USART3_MspInit 1 */
  }
}

void HAL_UART_MspDeInit(UART_HandleTypeDef* uartHandle)
{

  if(uartHandle->Instance==USART1)
  {
  /* USER CODE BEGIN USART1_MspDeInit 0 */

  /* USER CODE END USART1_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_USART1_CLK_DISABLE();

    /**USART1 GPIO Configuration
    PA9     ------> USART1_TX
    PA10     ------> USART1_RX
    */
    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_9|GPIO_PIN_10);

    /* USART1 DMA DeInit */
    HAL_DMA_DeInit(uartHandle->hdmatx);

    /* USART1 interrupt Deinit */
    HAL_NVIC_DisableIRQ(USART1_IRQn);
  /* USER CODE BEGIN USART1_MspDeInit 1 */

  /* USER CODE END USART1_MspDeInit 1 */
  }
  else if(uartHandle->Instance==USART3)
  {
  /* USER CODE BEGIN USART3_MspDeInit 0 */

  /* USER CODE END USART3_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_USART3_CLK_DISABLE();

    /**USART3 GPIO Configuration
    PB10     ------> USART3_TX
    PB11     ------> USART3_RX
    */
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_10|GPIO_PIN_11);

    /* USART3 DMA DeInit */
    HAL_DMA_DeInit(uartHandle->hdmarx);

    /* USART3 interrupt Deinit */
    HAL_NVIC_DisableIRQ(USART3_IRQn);
  /* USER CODE BEGIN USART3_MspDeInit 1 */

  /* USER CODE END USART3_MspDeInit 1 */
  }
}

/* USER CODE BEGIN 1 */
volatile uint8_t usart_dma_tx_over = 1; /* 串口 DMA 发�?�完成标�? */

/**
 * @brief 计算字符串长度（兼容转义字符�?
 * @param str 指向要计算长度的字符串的指针
 * @return 返回字符串的长度
 */
uint16_t calculateStringLength(const uint8_t *str)
{
  int length = 0;
  while (*str)
  {
    if (*str == '\\')
    {
      if (*(str + 1) != '\0')
      {
        length += 2;
        str++;
      }
      else
      {
        length++;
      }
    }
    else
    {
      length++;
    }
    str++;
  }
  return length;
}

/**
 * @brief USART1 使用 DMA 发�?�字符串（保留旧接口，未加锁�?
 */
void USART1_TX_DMA_String(uint8_t *pBuf)
{
  for (volatile uint16_t i = 0; i < 5000 && (!usart_dma_tx_over); i++);  /* 等待前一�? DMA 发�?�完�? */
  usart_dma_tx_over = 0;
  HAL_UART_Transmit_DMA(&huart1, pBuf, calculateStringLength(pBuf));
}

/**
 * @brief USART1_Printf 函数（线程安全版�?
 *
 * 使用 USART1 串口进行打印输出�?
 * [FIX-13] FreeRTOS 互斥锁串行化 + ISR 保护�?
 *   - 任务上下文：取锁后等待前�?�? DMA 完成（持锁等待不会与其他
 *     任务并发写共享缓�?/标志），构�?�报文�?�启�? DMA，最后释放锁�?
 *   - ISR 上下文（�? HardFault 打印）：xSemaphoreTake 在中断中非法�?
 *     直接等待 DMA 完成后发送，不取锁�??
 * @return 成功打印的字符数；失败返�? -1
 */
int USART1_Printf(const char *format, ...)
{
  va_list arg;
  static char SendBuff[200];
  int rv;
  int in_isr;

  /* 互斥锁惰性创建（首个调用者创建；临界区防并发重复创建�? */
  if (s_uart_mtx == NULL)
  {
    taskENTER_CRITICAL();
    if (s_uart_mtx == NULL)
    {
      s_uart_mtx = xSemaphoreCreateMutex();
    }
    taskEXIT_CRITICAL();
  }

  in_isr = xPortIsInsideInterrupt();

  if (!in_isr && s_uart_mtx != NULL)
  {
    if (xSemaphoreTake(s_uart_mtx, pdMS_TO_TICKS(200)) != pdTRUE)
    {
      return -1;   /* 拿不到锁：丢弃本次日志，避免无限等待 */
    }
    /* 持锁等待前一�? DMA 完成（DMA 完成回调是中断，不需要锁�? */
    while (!usart_dma_tx_over)
    {
      vTaskDelay(1);
    }
  }
  else
  {
    /* ISR 上下文：等待 DMA 完成 */
    while (!usart_dma_tx_over);
  }

  va_start(arg, format);
  rv = vsnprintf(SendBuff, sizeof(SendBuff), format, arg);
  va_end(arg);

  if (rv <= 0)
  {
    if (!in_isr && s_uart_mtx != NULL) xSemaphoreGive(s_uart_mtx);
    return rv;
  }
  if (rv >= (int)sizeof(SendBuff))
  {
    rv = sizeof(SendBuff) - 1;
  }

  usart_dma_tx_over = 0;
  HAL_UART_Transmit_DMA(&huart1, (uint8_t *)SendBuff, (uint16_t)rv);

  if (!in_isr && s_uart_mtx != NULL)
  {
    xSemaphoreGive(s_uart_mtx);
  }
  return rv;
}

/**
 * @brief UART 传输完成回调函数
 * @param huart UART 句柄指针
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    usart_dma_tx_over = 1;
  }
}
/* USER CODE END 1 */
