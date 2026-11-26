/*
 * App_LED.h
 *
 * App LED: chớp LED_DEBUG (PE2) với chu kỳ 1 s (sáng 500 ms, tắt 500 ms).
 * Định thời do scheduler đảm nhiệm: task được gọi mỗi APP_LED_TOGGLE_MS.
 */

#ifndef APP_APP_LED_H_
#define APP_APP_LED_H_

#include <stdint.h>

#define APP_LED_PERIOD_MS     1000U                      /* chu kỳ chớp */
#define APP_LED_TOGGLE_MS     (APP_LED_PERIOD_MS / 2U)   /* chu kỳ gọi task = nửa chu kỳ chớp */

void App_LED_Init(void);
void App_LED_Task(void *pvParam);

#endif /* APP_APP_LED_H_ */
