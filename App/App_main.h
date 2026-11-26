/*
 * App_main.h
 *
 * Tầng App: khởi tạo từng app và chạy scheduler.
 */

#ifndef APP_APP_MAIN_H_
#define APP_APP_MAIN_H_

#include <stdint.h>
#include "scheduler.h"

void App_Init(void);
void App_Run(void);
void App_SysTick_Handler(void);

#endif /* APP_APP_MAIN_H_ */
