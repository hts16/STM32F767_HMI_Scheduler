/*
 * App_LED.c
 *
 * Chớp LED_DEBUG (PE2) chu kỳ 1 s. Scheduler gọi App_LED_Task mỗi
 * APP_LED_TOGGLE_MS (500 ms), mỗi lần gọi đổi trạng thái LED một lần.
 * Không phụ thuộc SD card.
 */

#include "App_LED.h"
#include "main.h"

typedef enum
{
  LED_OFF = 0,
  LED_ON
} led_state_t;

static led_state_t led_state;

void App_LED_Init(void)
{
  led_state = LED_OFF;
  LL_GPIO_ResetOutputPin(LED_DEBUG_GPIO_Port, LED_DEBUG_Pin);
}

void App_LED_Task(void *pvParam)
{
  (void)pvParam;

  switch (led_state)
  {
    case LED_OFF:
      LL_GPIO_SetOutputPin(LED_DEBUG_GPIO_Port, LED_DEBUG_Pin);
      led_state = LED_ON;
      break;

    case LED_ON:
    default:
      LL_GPIO_ResetOutputPin(LED_DEBUG_GPIO_Port, LED_DEBUG_Pin);
      led_state = LED_OFF;
      break;
  }
}
