/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    periph_test.h
  * @brief   Test từng ngoại vi: LED, SDRAM, SDCARD, LCD.
  *          Chọn bài test bằng TEST_SELECT trong main.h.
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __PERIPH_TEST_H
#define __PERIPH_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* Chạy bài test được chọn bởi TEST_SELECT (không trả về). */
void Periph_Test_Run(void);

#ifdef __cplusplus
}
#endif

#endif /* __PERIPH_TEST_H */
