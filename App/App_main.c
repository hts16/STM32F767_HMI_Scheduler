/*
 * App_main.c
 *
 * Khởi tạo các app và khai báo bảng task cho scheduler.
 * Mỗi app tự quản lý trạng thái của nó trong file .c riêng; chu kỳ gọi
 * task được khai báo tập trung ở bảng dưới đây.
 */

#include "App_main.h"
#include "main.h"
#include "App_LED.h"
#include "App_LCD.h"
#include "App_SDCARD.h"
#include "App_TOUCH.h"

/* Giữ log khởi động trên màn hình trước khi hiện ảnh (đặt 0 để bỏ) */
#define APP_BOOT_LOG_HOLD_MS   0U

/* Bảng task của scheduler. ui32FrequencyTicks tính theo tick 1 kHz (1 tick = 1 ms).
 * Thứ tự: hàm, tham số, chu kỳ (tick), lần gọi cuối, đang bật. */
tSchedulerTask g_psSchedulerTable[] =
{
  { &App_LED_Task,    (void *)0, APP_LED_TOGGLE_MS,  0, true },   /* LED: đổi trạng thái mỗi 500 ms */
  { &App_TOUCH_Task,  (void *)0, APP_TOUCH_TASK_MS,  0, true },   /* TOUCH: xử lý ngắt GT911 mỗi 20 ms */
  { &App_SDCARD_Task, (void *)0, APP_SDCARD_TASK_MS, 0, true },   /* SD: chạm -> ảnh kế tiếp */
};

uint32_t g_ui32SchedulerNumTasks = sizeof(g_psSchedulerTable) / sizeof(g_psSchedulerTable[0]);

void App_Init(void)
{
  App_LED_Init();
  App_LCD_Init();        /* panel, đèn nền, SDRAM, 2 framebuffer */
  App_TOUCH_Init();      /* reset chip touch, dò I2C1, in kết quả */
  HAL_Delay(APP_BOOT_LOG_HOLD_MS);
  App_SDCARD_Init();     /* mount thẻ, liệt kê ảnh, hiện ảnh đầu tiên */
  SchedulerInit(1000);   /* SysTick 1 kHz */
}

void App_Run(void)
{
  SchedulerRun();
}

/* Gọi từ SysTick_Handler, mỗi 1 ms */
void App_SysTick_Handler(void)
{
  SchedulerSysTickIntHandler();
}
