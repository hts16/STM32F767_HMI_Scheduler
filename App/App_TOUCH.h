/*
 * App_TOUCH.h
 *
 * App TOUCH: cảm ứng điện dung trên FFC chân 37..40
 *   37 TOUCH_SDA -> PB7 (I2C1_SDA)    38 TOUCH_SCL -> PB6 (I2C1_SCL)
 *   39 TOUCH_RST -> PB5 (GPIO out)    40 TOUCH_INT -> PB4 (GPIO in)
 * I2C1 do MX_I2C1_Init (CubeMX) khởi tạo; app này reset chip touch, dò địa chỉ,
 * in kết quả lên màn hình. TOUCH_INT là ngắt EXTI4 (cả 2 cạnh): mỗi lần có ngắt,
 * task đọc trạng thái GT911 và báo "tap" khi chuyển từ không chạm sang chạm.
 */

#ifndef APP_APP_TOUCH_H_
#define APP_APP_TOUCH_H_

#include <stdint.h>

#define TOUCH_ADDR_GT911_A    0x5DU   /* GT911, INT = 0 khi nhả RST */
#define TOUCH_ADDR_GT911_B    0x14U   /* GT911, INT = 1 khi nhả RST */
#define TOUCH_ADDR_FT5X06     0x38U   /* FocalTech FT5x06 / FT6x06 */

typedef enum
{
  TOUCH_CHIP_NONE = 0,
  TOUCH_CHIP_GT911,
  TOUCH_CHIP_FT5X06,
} touch_chip_t;

#define APP_TOUCH_TASK_MS     20U     /* chu kỳ task xử lý cảm ứng */

void    App_TOUCH_Init(void);
void    App_TOUCH_Task(void *pvParam);
void    App_TOUCH_IRQHandler(void);   /* gọi từ EXTI4_IRQHandler (TOUCH_INT, PB4) */
uint8_t App_TOUCH_GetTap(void);       /* 1 nếu có lần chạm mới kể từ lần gọi trước */

#endif /* APP_APP_TOUCH_H_ */
