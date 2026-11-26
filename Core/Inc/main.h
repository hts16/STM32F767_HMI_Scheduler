/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
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
#include "stm32f7xx_hal.h"

#include "stm32f7xx_ll_rcc.h"
#include "stm32f7xx_ll_bus.h"
#include "stm32f7xx_ll_system.h"
#include "stm32f7xx_ll_exti.h"
#include "stm32f7xx_ll_cortex.h"
#include "stm32f7xx_ll_utils.h"
#include "stm32f7xx_ll_pwr.h"
#include "stm32f7xx_ll_dma.h"
#include "stm32f7xx_ll_tim.h"
#include "stm32f7xx_ll_gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

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
#define LED_DEBUG_Pin LL_GPIO_PIN_2
#define LED_DEBUG_GPIO_Port GPIOE
#define LTDC_TFT_EN_Pin LL_GPIO_PIN_12
#define LTDC_TFT_EN_GPIO_Port GPIOD
#define LED_PWM_Pin LL_GPIO_PIN_13
#define LED_PWM_GPIO_Port GPIOD
#define SD_DET_Pin LL_GPIO_PIN_4
#define SD_DET_GPIO_Port GPIOD
#define TOUCH_INT_Pin LL_GPIO_PIN_4
#define TOUCH_INT_GPIO_Port GPIOB
#define TOUCH_RST_Pin LL_GPIO_PIN_5
#define TOUCH_RST_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */
/* Chọn bài test ngoại vi: 0 = chương trình chính, 1 = LED, 2 = SDRAM,
   3 = SDCARD, 4 = LCD. Mỗi bài test chạy một lần rồi nháy LED báo kết quả. */
#define TEST_SELECT		0

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
