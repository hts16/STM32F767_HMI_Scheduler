/*
 * App_TOUCH.c
 *
 * Khởi tạo cảm ứng:
 *  1. Reset chip touch qua TOUCH_RST (PB5), giữ TOUCH_INT (PB4) = 0 lúc nhả reset
 *     để GT911 chọn địa chỉ 0x5D (giống driver project tham khảo HMI_STM32F767).
 *  2. Dò thiết bị trên I2C1 ở các địa chỉ 0x5D, 0x14 (GT911) và 0x38 (FT5x06).
 *  3. GT911: đọc Product ID (0x8140) và độ phân giải cấu hình (0x8048).
 *  4. In kết quả lên màn hình (App_LCD_Log).
 *
 * Khi chạy: ngắt TOUCH_INT (EXTI4) chỉ bật cờ; App_TOUCH_Task (mỗi 20 ms) đọc
 * thanh ghi trạng thái 0x814E qua I2C, xoá trạng thái, và phát hiện lần chạm mới
 * (không chạm -> chạm). App khác lấy sự kiện qua App_TOUCH_GetTap().
 */

#include <string.h>

#include "App_TOUCH.h"
#include "App_LCD.h"
#include "main.h"

extern I2C_HandleTypeDef hi2c1;

#define GT911_REG_PROD_ID    0x8140U
#define GT911_REG_X_MAX      0x8048U
#define GT911_REG_MOD_SW1    0x804DU   /* bit1..0: kiểu ngắt INT */
#define GT911_REG_STATUS     0x814EU   /* bit7: có dữ liệu, bit3..0: số điểm chạm */
#define GT911_REG_POINT1     0x8150U   /* X lo, X hi, Y lo, Y hi */
#define TOUCH_RELEASE_MS     150U      /* không có ngắt trong khoảng này -> coi như nhả tay */
#define I2C_TIMEOUT_MS       30U

/* Trạng thái để xem bằng debugger (Live Expressions) */
volatile touch_chip_t g_touch_chip;
volatile uint8_t      g_touch_addr;     /* địa chỉ 7-bit tìm thấy, 0 = không có */
volatile uint32_t     g_touch_i2c_err;  /* hi2c1.ErrorCode lần lỗi gần nhất */
volatile uint32_t     g_touch_irq_count;/* số lần ngắt TOUCH_INT */
volatile uint32_t     g_touch_tap_count;/* số lần chạm mới */
volatile uint16_t     g_touch_x, g_touch_y;   /* toạ độ điểm 1 gần nhất */
volatile uint8_t      g_touch_down;     /* 1 = đang chạm */

static volatile uint8_t touch_irq_flag;
static volatile uint8_t touch_tap_pending;
static uint32_t         touch_last_ms;

/* Reset chip touch, chọn địa chỉ GT911 = 0x5D */
static void touch_hw_reset(void)
{
  /* INT tạm thời là output để giữ mức 0 */
  LL_GPIO_SetPinMode(TOUCH_INT_GPIO_Port, TOUCH_INT_Pin, LL_GPIO_MODE_OUTPUT);
  LL_GPIO_SetPinOutputType(TOUCH_INT_GPIO_Port, TOUCH_INT_Pin, LL_GPIO_OUTPUT_PUSHPULL);
  LL_GPIO_ResetOutputPin(TOUCH_INT_GPIO_Port, TOUCH_INT_Pin);

  LL_GPIO_ResetOutputPin(TOUCH_RST_GPIO_Port, TOUCH_RST_Pin);
  HAL_Delay(20U);
  LL_GPIO_SetOutputPin(TOUCH_RST_GPIO_Port, TOUCH_RST_Pin);
  HAL_Delay(60U);

  /* Trả INT về input cho chip touch điều khiển, chờ chip nạp cấu hình */
  LL_GPIO_SetPinMode(TOUCH_INT_GPIO_Port, TOUCH_INT_Pin, LL_GPIO_MODE_INPUT);
  HAL_Delay(100U);
}

static uint8_t touch_probe(uint8_t addr7)
{
  return (HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(addr7 << 1), 3U, I2C_TIMEOUT_MS) == HAL_OK) ? 1U : 0U;
}

void App_TOUCH_Init(void)
{
  static const uint8_t addrs[] = { TOUCH_ADDR_GT911_A, TOUCH_ADDR_GT911_B, TOUCH_ADDR_FT5X06 };
  uint32_t i;

  g_touch_chip = TOUCH_CHIP_NONE;
  g_touch_addr = 0U;
  g_touch_i2c_err = 0U;

  App_LCD_Log(LCD_COLOR_WHITE, "TOUCH RESET + I2C1 SCAN (PB6/PB7) ...");
  touch_hw_reset();

  for (i = 0U; i < sizeof(addrs); i++)
  {
    if (touch_probe(addrs[i]) != 0U)
    {
      g_touch_addr = addrs[i];
      break;
    }
  }

  if (g_touch_addr == 0U)
  {
    g_touch_i2c_err = hi2c1.ErrorCode;
    App_LCD_Log(LCD_COLOR_RED, "TOUCH NOT FOUND (0x5D/0x14/0x38), err=0x%08lX",
                (unsigned long)g_touch_i2c_err);
    App_LCD_Log(LCD_COLOR_RED, "  SCL=%u SDA=%u INT=%u RST=%u",
                (unsigned)LL_GPIO_IsInputPinSet(GPIOB, LL_GPIO_PIN_6),
                (unsigned)LL_GPIO_IsInputPinSet(GPIOB, LL_GPIO_PIN_7),
                (unsigned)LL_GPIO_IsInputPinSet(TOUCH_INT_GPIO_Port, TOUCH_INT_Pin),
                (unsigned)LL_GPIO_IsOutputPinSet(TOUCH_RST_GPIO_Port, TOUCH_RST_Pin));
    App_LCD_Log(LCD_COLOR_YELLOW, "  -> check FFC pin 37..40, pull-up SCL/SDA");
    return;
  }

  if (g_touch_addr == TOUCH_ADDR_FT5X06)
  {
    g_touch_chip = TOUCH_CHIP_FT5X06;
    App_LCD_Log(LCD_COLOR_GREEN, "TOUCH I2C OK: FT5x06 @0x%02X", (unsigned)g_touch_addr);
    return;
  }

  /* GT911: đọc Product ID và độ phân giải */
  {
    uint8_t id[5] = { 0 };
    uint8_t res[4] = { 0 };

    g_touch_chip = TOUCH_CHIP_GT911;
    if ((HAL_I2C_Mem_Read(&hi2c1, (uint16_t)(g_touch_addr << 1), GT911_REG_PROD_ID,
                          I2C_MEMADD_SIZE_16BIT, id, 4U, I2C_TIMEOUT_MS) != HAL_OK) ||
        (HAL_I2C_Mem_Read(&hi2c1, (uint16_t)(g_touch_addr << 1), GT911_REG_X_MAX,
                          I2C_MEMADD_SIZE_16BIT, res, 4U, I2C_TIMEOUT_MS) != HAL_OK))
    {
      g_touch_i2c_err = hi2c1.ErrorCode;
      App_LCD_Log(LCD_COLOR_YELLOW, "TOUCH @0x%02X ACK, read fault err=0x%08lX",
                  (unsigned)g_touch_addr, (unsigned long)g_touch_i2c_err);
      return;
    }

    for (i = 0U; i < 4U; i++)
    {
      if ((id[i] < 0x20U) || (id[i] > 0x7EU))
      {
        id[i] = '\0';   /* chỉ giữ ký tự in được */
      }
    }
    App_LCD_Log(LCD_COLOR_GREEN, "TOUCH I2C OK: GT%s @0x%02X, %ux%u", (const char *)id,
                (unsigned)g_touch_addr,
                (unsigned)(res[0] | ((uint16_t)res[1] << 8)),
                (unsigned)(res[2] | ((uint16_t)res[3] << 8)));
  }

  /* Kiểu ngắt INT của GT911 (EXTI4 đã bắt cả 2 cạnh nên kiểu nào cũng nhận được) */
  {
    static const char *const mode[] = { "RISING", "FALLING", "LOW LEVEL", "HIGH LEVEL" };
    uint8_t sw1 = 0U;

    if (HAL_I2C_Mem_Read(&hi2c1, (uint16_t)(g_touch_addr << 1), GT911_REG_MOD_SW1,
                         I2C_MEMADD_SIZE_16BIT, &sw1, 1U, I2C_TIMEOUT_MS) == HAL_OK)
    {
      App_LCD_Log(LCD_COLOR_GREEN, "  INT mode: %s -> EXTI4 (PB4) enabled", mode[sw1 & 0x03U]);
    }
  }

  /* Xoá trạng thái cũ và cờ ngắt phát sinh trong lúc reset */
  {
    uint8_t zero = 0U;
    (void)HAL_I2C_Mem_Write(&hi2c1, (uint16_t)(g_touch_addr << 1), GT911_REG_STATUS,
                            I2C_MEMADD_SIZE_16BIT, &zero, 1U, I2C_TIMEOUT_MS);
  }
  touch_irq_flag = 0U;
  touch_tap_pending = 0U;
  g_touch_down = 0U;
}

/* Gọi trong ngắt EXTI4: chỉ đánh dấu, đọc I2C để task làm */
void App_TOUCH_IRQHandler(void)
{
  touch_irq_flag = 1U;
  g_touch_irq_count++;
}

void App_TOUCH_Task(void *pvParam)
{
  uint8_t status = 0U;
  uint8_t zero = 0U;
  uint8_t pt[4];
  uint32_t now = HAL_GetTick();

  (void)pvParam;

  if (g_touch_chip != TOUCH_CHIP_GT911)
  {
    return;
  }

  if (touch_irq_flag == 0U)
  {
    /* Không còn ngắt một lúc khi đang chạm -> coi như đã nhả tay */
    if ((g_touch_down != 0U) && ((now - touch_last_ms) > TOUCH_RELEASE_MS))
    {
      g_touch_down = 0U;
    }
    return;
  }
  touch_irq_flag = 0U;

  if (HAL_I2C_Mem_Read(&hi2c1, (uint16_t)(g_touch_addr << 1), GT911_REG_STATUS,
                       I2C_MEMADD_SIZE_16BIT, &status, 1U, I2C_TIMEOUT_MS) != HAL_OK)
  {
    g_touch_i2c_err = hi2c1.ErrorCode;
    return;
  }
  if ((status & 0x80U) == 0U)
  {
    return;   /* chưa có dữ liệu mới */
  }

  if ((status & 0x0FU) != 0U)
  {
    if (HAL_I2C_Mem_Read(&hi2c1, (uint16_t)(g_touch_addr << 1), GT911_REG_POINT1,
                         I2C_MEMADD_SIZE_16BIT, pt, 4U, I2C_TIMEOUT_MS) == HAL_OK)
    {
      g_touch_x = (uint16_t)(pt[0] | ((uint16_t)pt[1] << 8));
      g_touch_y = (uint16_t)(pt[2] | ((uint16_t)pt[3] << 8));
    }
    if (g_touch_down == 0U)
    {
      g_touch_down = 1U;
      touch_tap_pending = 1U;   /* chạm mới */
      g_touch_tap_count++;
    }
    touch_last_ms = now;
  }
  else
  {
    g_touch_down = 0U;          /* GT911 báo 0 điểm: đã nhả tay */
  }

  /* Báo GT911 đã đọc xong để nó cập nhật lần quét kế tiếp */
  (void)HAL_I2C_Mem_Write(&hi2c1, (uint16_t)(g_touch_addr << 1), GT911_REG_STATUS,
                          I2C_MEMADD_SIZE_16BIT, &zero, 1U, I2C_TIMEOUT_MS);
}

uint8_t App_TOUCH_GetTap(void)
{
  if (touch_tap_pending != 0U)
  {
    touch_tap_pending = 0U;
    return 1U;
  }
  return 0U;
}
