/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    periph_test.c
  * @brief   Bài test từng ngoại vi: LED, SDRAM, SDCARD, LCD.
  *
  *  TEST_SELECT (main.h):
  *    0 = chương trình chính (không test)
  *    1 = LED: nháy LED_DEBUG (PE2) và quét độ sáng backlight (TIM4_CH2 / PD13)
  *    2 = SDRAM: ghi/đọc mẫu 32-bit và kiểm tra byte-lane (NBL0/NBL1)
  *    3 = SDCARD: đọc block 0 (và ghi thử nếu SDCARD_TEST_WRITE = 1)
  *    4 = LCD: vẽ thanh màu lên framebuffer nằm trong SDRAM
  *
  *  Báo kết quả bằng LED_DEBUG (PE2):
  *    - PASS: nháy chậm 1 Hz
  *    - FAIL: nháy nhanh n lần rồi nghỉ (n = mã lỗi)
  *    - Lỗi khởi tạo HAL: nháy rất nhanh (xem Error_Handler trong main.c)
  ******************************************************************************
  */
/* USER CODE END Header */

#include "periph_test.h"
#include "bsp_driver_sd.h"

/* ---------------- Cấu hình ---------------- */
#define SDRAM_BASE_ADDR        0xC0000000UL   /* FMC SDRAM bank 1 (SDNE0 = PC2) */
#define LCD_WIDTH              800U
#define LCD_HEIGHT             480U
#define LCD_FB_ADDR            SDRAM_BASE_ADDR                 /* framebuffer RGB565, đầu SDRAM */
#define SDRAM_TEST_ADDR        (SDRAM_BASE_ADDR + 0x400000UL)  /* vùng test, cách framebuffer 4 MB */
#define SDRAM_TEST_WORDS       (256UL * 1024UL)                /* 1 MB */
#define SDRAM_REFRESH_COUNT    1542U                            /* 64 ms / 4096 hàng @ SDCLK 100 MHz */

/* Ghi thử thẻ SD: 1 = ghi vào SDCARD_TEST_BLOCK rồi khôi phục dữ liệu cũ.
   Chỉ bật khi đã chắc block đó không chứa dữ liệu quan trọng. */
#define SDCARD_TEST_WRITE      0
#define SDCARD_TEST_BLOCK      0x00040000UL                    /* ~128 MB từ đầu thẻ */

/* Mode register của IS42S16400N: burst length 1, tuần tự, CAS latency 3,
   chế độ chuẩn, ghi đơn (single write). */
#define SDRAM_MR_BURST_LEN_1       0x0000U
#define SDRAM_MR_BURST_SEQ         0x0000U
#define SDRAM_MR_CAS_LAT_3         0x0030U
#define SDRAM_MR_OP_STANDARD       0x0000U
#define SDRAM_MR_WRITE_SINGLE      0x0200U

/* Các handle do main.c khai báo */
extern LTDC_HandleTypeDef hltdc;
extern SDRAM_HandleTypeDef hsdram1;
extern SD_HandleTypeDef hsd1;

/* ---------------- Tiện ích ---------------- */
static void LED_Set(uint8_t on)
{
  if (on)
  {
    LL_GPIO_SetOutputPin(LED_DEBUG_GPIO_Port, LED_DEBUG_Pin);
  }
  else
  {
    LL_GPIO_ResetOutputPin(LED_DEBUG_GPIO_Port, LED_DEBUG_Pin);
  }
}

/* Không trả về. code = 0: PASS (nháy chậm); code > 0: FAIL (nháy code lần). */
static void Blink_Result(uint32_t code)
{
  while (1)
  {
    if (code == 0U)
    {
      LED_Set(1); HAL_Delay(500);
      LED_Set(0); HAL_Delay(500);
    }
    else
    {
      for (uint32_t i = 0; i < code; i++)
      {
        LED_Set(1); HAL_Delay(150);
        LED_Set(0); HAL_Delay(150);
      }
      HAL_Delay(1500);
    }
  }
}

/* Backlight: TIM4_CH2 đã cấu hình PWM1 (PD13). ARR = 65535. */
static void Backlight_Init(void)
{
  LL_TIM_OC_SetCompareCH2(TIM4, 0U);
  LL_TIM_CC_EnableChannel(TIM4, LL_TIM_CHANNEL_CH2);
  LL_TIM_EnableCounter(TIM4);
}

static void Backlight_Set(uint32_t duty)
{
  LL_TIM_OC_SetCompareCH2(TIM4, duty);
}

/* ---------------- SDRAM ---------------- */
/* Chuỗi khởi tạo SDRAM theo datasheet: clock enable -> PALL -> auto-refresh x8
   -> load mode register -> đặt chu kỳ refresh. */
static HAL_StatusTypeDef SDRAM_Init_Sequence(void)
{
  FMC_SDRAM_CommandTypeDef cmd = {0};
  HAL_StatusTypeDef st;

  cmd.CommandTarget = FMC_SDRAM_CMD_TARGET_BANK1;
  cmd.AutoRefreshNumber = 1U;
  cmd.ModeRegisterDefinition = 0U;

  cmd.CommandMode = FMC_SDRAM_CMD_CLK_ENABLE;
  st = HAL_SDRAM_SendCommand(&hsdram1, &cmd, 0xFFFFU);
  if (st != HAL_OK) return st;
  HAL_Delay(1U);                       /* chờ >= 100 us sau khi bật clock */

  cmd.CommandMode = FMC_SDRAM_CMD_PALL;
  st = HAL_SDRAM_SendCommand(&hsdram1, &cmd, 0xFFFFU);
  if (st != HAL_OK) return st;

  cmd.CommandMode = FMC_SDRAM_CMD_AUTOREFRESH_MODE;
  cmd.AutoRefreshNumber = 8U;
  st = HAL_SDRAM_SendCommand(&hsdram1, &cmd, 0xFFFFU);
  if (st != HAL_OK) return st;

  cmd.CommandMode = FMC_SDRAM_CMD_LOAD_MODE;
  cmd.AutoRefreshNumber = 1U;
  cmd.ModeRegisterDefinition = SDRAM_MR_BURST_LEN_1 | SDRAM_MR_BURST_SEQ |
                               SDRAM_MR_CAS_LAT_3 | SDRAM_MR_OP_STANDARD |
                               SDRAM_MR_WRITE_SINGLE;
  st = HAL_SDRAM_SendCommand(&hsdram1, &cmd, 0xFFFFU);
  if (st != HAL_OK) return st;

  return HAL_SDRAM_ProgramRefreshRate(&hsdram1, SDRAM_REFRESH_COUNT);
}

/* Trả về: 0 = PASS, 1 = lỗi khởi tạo, 2 = sai dữ liệu */
static uint32_t Test_SDRAM(void)
{
  volatile uint32_t *m32 = (volatile uint32_t *)SDRAM_TEST_ADDR;
  volatile uint8_t  *m8  = (volatile uint8_t  *)SDRAM_TEST_ADDR;
  uint32_t errors = 0U;
  uint32_t i;

  if (SDRAM_Init_Sequence() != HAL_OK) return 1U;

  /* 1) Ghi rồi đọc lại mẫu băm (kiểm tra đường dữ liệu và địa chỉ) */
  for (i = 0U; i < SDRAM_TEST_WORDS; i++)
  {
    m32[i] = (i * 2654435761UL) ^ 0xA5A5A5A5UL;
  }
  SCB_CleanInvalidateDCache();   /* SDRAM được cache: buộc đọc lại từ chip */
  for (i = 0U; i < SDRAM_TEST_WORDS; i++)
  {
    if (m32[i] != ((i * 2654435761UL) ^ 0xA5A5A5A5UL)) errors++;
  }

  /* 2) Ghi từng byte: mỗi byte chỉ bật đúng một NBL (byte-lane).
        Byte chẵn -> NBL0 (D0..D7), byte lẻ -> NBL1 (D8..D15).
        Word kết quả phải là 0x44332211. */
  for (i = 0U; i < 4096U; i++)
  {
    m32[i] = 0U;
    m8[4U * i + 0U] = 0x11U;
    m8[4U * i + 1U] = 0x22U;
    m8[4U * i + 2U] = 0x33U;
    m8[4U * i + 3U] = 0x44U;
    SCB_CleanInvalidateDCache_by_Addr((uint32_t *)&m32[i], 4);
    if (m32[i] != 0x44332211UL) errors++;
  }

  return (errors == 0U) ? 0U : 2U;
}

/* ---------------- SDCARD ---------------- */
/* Trả về: 0 = PASS, 2 = không đọc được thông tin thẻ, 3 = lỗi đọc,
           4 = dữ liệu ghi thử không khớp */
static uint32_t Test_SDCARD(void)
{
  static uint8_t buf[512];
  static uint8_t save[512];
  static uint8_t chk[512];
  HAL_SD_CardInfoTypeDef info;
  uint32_t i;
  uint32_t timeout;

  /* Khởi tạo thẻ qua BSP của FATFS (MX_SDMMC1_SD_Init chỉ gán thông số) */
  if (BSP_SD_Init() != MSD_OK) return 2U;
  if (HAL_SD_GetCardInfo(&hsd1, &info) != HAL_OK) return 2U;
  if (info.BlockNbr == 0U) return 2U;

  /* Đọc block 0 (MBR / boot sector) */
  if (HAL_SD_ReadBlocks(&hsd1, buf, 0U, 1U, 1000U) != HAL_OK) return 3U;

#if SDCARD_TEST_WRITE
  /* Lưu block cũ, ghi mẫu, đọc lại so sánh, rồi khôi phục */
  if (HAL_SD_ReadBlocks(&hsd1, save, SDCARD_TEST_BLOCK, 1U, 1000U) != HAL_OK) return 3U;

  for (i = 0U; i < 512U; i++) buf[i] = (uint8_t)(i ^ 0x5AU);
  if (HAL_SD_WriteBlocks(&hsd1, buf, SDCARD_TEST_BLOCK, 1U, 1000U) != HAL_OK) return 4U;

  /* Chờ thẻ ra khỏi trạng thái bận */
  timeout = 100000U;
  while ((HAL_SD_GetCardState(&hsd1) != HAL_SD_CARD_TRANSFER) && (timeout-- > 0U)) { }

  if (HAL_SD_ReadBlocks(&hsd1, chk, SDCARD_TEST_BLOCK, 1U, 1000U) != HAL_OK) return 4U;
  for (i = 0U; i < 512U; i++)
  {
    if (chk[i] != buf[i]) { /* khôi phục trước khi báo lỗi */
      (void)HAL_SD_WriteBlocks(&hsd1, save, SDCARD_TEST_BLOCK, 1U, 1000U);
      return 4U;
    }
  }

  /* Khôi phục dữ liệu cũ */
  if (HAL_SD_WriteBlocks(&hsd1, save, SDCARD_TEST_BLOCK, 1U, 1000U) != HAL_OK) return 4U;
#endif

  return 0U;
}

/* ---------------- LCD ---------------- */
static void Test_LCD(void)
{
  static const uint16_t bars[8] = {
    0xF800U, /* đỏ    */ 0x07E0U, /* xanh lá */ 0x001FU, /* xanh dương */
    0xFFFFU, /* trắng */ 0x0000U, /* đen     */ 0xFFE0U, /* vàng    */
    0x07FFU, /* lục lam */ 0xF81FU  /* tím  */
  };
  volatile uint16_t *fb = (volatile uint16_t *)LCD_FB_ADDR;
  uint32_t x, y;

  if (SDRAM_Init_Sequence() != HAL_OK)
  {
    Blink_Result(1U);
  }

  /* Layer 0: RGB565, cửa sổ 800x480 tại (0,0), framebuffer ở đầu SDRAM */
  HAL_LTDC_SetPixelFormat(&hltdc, LTDC_PIXEL_FORMAT_RGB565, 0U);
  HAL_LTDC_SetAddress(&hltdc, LCD_FB_ADDR, 0U);
  HAL_LTDC_SetWindowPosition(&hltdc, 0U, 0U, 0U);
  HAL_LTDC_SetWindowSize(&hltdc, LCD_WIDTH, LCD_HEIGHT, 0U);
  HAL_LTDC_SetAlpha(&hltdc, 255U, 0U);

  /* 8 thanh màu dọc */
  for (y = 0U; y < LCD_HEIGHT; y++)
  {
    for (x = 0U; x < LCD_WIDTH; x++)
    {
      fb[y * LCD_WIDTH + x] = bars[(x * 8U) / LCD_WIDTH];
    }
  }

  /* 40 hàng dưới cùng: dải xám 0..255 để kiểm tra độ tuyến tính */
  for (y = LCD_HEIGHT - 40U; y < LCD_HEIGHT; y++)
  {
    for (x = 0U; x < LCD_WIDTH; x++)
    {
      uint32_t g = (x * 255U) / (LCD_WIDTH - 1U);
      fb[y * LCD_WIDTH + x] = (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
    }
  }
  SCB_CleanDCache();                  /* SDRAM được cache: đẩy hình xuống cho LTDC đọc */

  Backlight_Init();
  Backlight_Set(0xFFFFU);             /* sáng tối đa */

  Blink_Result(0U);                   /* LED nháy chậm = đã vẽ xong */
}

/* ---------------- LED ---------------- */
static void Test_LED(void)
{
  int32_t d;

  Backlight_Init();
  while (1)
  {
    /* Quét độ sáng backlight lên rồi xuống, LED_DEBUG nháy theo.
       Dùng int32_t để vòng lặp xuống không bị tràn số. */
    for (d = 0; d <= 0xFFFF; d += 0x0400)
    {
      Backlight_Set((uint32_t)d);
      LED_Set((d & 0x0400) != 0);
      HAL_Delay(15U);
    }
    for (d = 0xFFFF; d >= 0; d -= 0x0400)
    {
      Backlight_Set((uint32_t)d);
      LED_Set((d & 0x0400) != 0);
      HAL_Delay(15U);
    }
  }
}

/* ---------------- Điều phối ---------------- */
void Periph_Test_Run(void)
{
#if (TEST_SELECT == 1)
  Test_LED();
#elif (TEST_SELECT == 2)
  Blink_Result(Test_SDRAM());
#elif (TEST_SELECT == 3)
  Blink_Result(Test_SDCARD());
#elif (TEST_SELECT == 4)
  Test_LCD();
#else
  /* TEST_SELECT = 0: chương trình chính, không làm gì thêm ở đây */
#endif
}
