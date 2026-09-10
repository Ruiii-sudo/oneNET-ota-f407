/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */
#define CCMRAM __attribute__((section("ccmram")))
/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define T_CS_Pin GPIO_PIN_13
#define T_CS_GPIO_Port GPIOC
#define LCD_CS_Pin GPIO_PIN_1
#define LCD_CS_GPIO_Port GPIOA
#define LCD_RST_Pin GPIO_PIN_2
#define LCD_RST_GPIO_Port GPIOA
#define LCD_RS_Pin GPIO_PIN_3
#define LCD_RS_GPIO_Port GPIOA
#define LCD_LED_Pin GPIO_PIN_4
#define LCD_LED_GPIO_Port GPIOA
#define T_IRQ_Pin GPIO_PIN_8
#define T_IRQ_GPIO_Port GPIOC
#define T_CLK_Pin GPIO_PIN_10
#define T_CLK_GPIO_Port GPIOC
#define T_DO_Pin GPIO_PIN_11
#define T_DO_GPIO_Port GPIOC
#define T_DIN_Pin GPIO_PIN_12
#define T_DIN_GPIO_Port GPIOC

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
