/*
 * App_LCD.c
 *
 * Khởi tạo hiển thị:
 *  1. Bật DISP (LTDC_TFT_EN, PD12) và đèn nền (TIM4_CH2, PD13, PWM 500 Hz,
 *     nối thẳng chân 35 FFC) ngay từ đầu, không phụ thuộc SDRAM.
 *  2. Khởi tạo SDRAM, kiểm tra ghi/đọc, xoá 2 framebuffer về màu đen.
 *  3. Layer 0 hiển thị FB0; app khác vẽ vào back buffer rồi gọi
 *     App_LCD_SwapBuffer() để đổi buffer ở vertical blanking (không bị xé hình).
 */

#include <stdarg.h>
#include <stdio.h>

#include "App_LCD.h"
#include "App_SDRAM.h"
#include "App_font8x16.h"
#include "main.h"

extern LTDC_HandleTypeDef hltdc;

static uint32_t lcd_front_addr;      /* buffer đang hiển thị */
static uint8_t  lcd_ready;
static uint32_t lcd_log_row;         /* dòng log kế tiếp */

/* Trạng thái để xem bằng debugger (Live Expressions) */
volatile uint32_t g_lcd_sdram_ok;    /* 1 = SDRAM ghi/đọc đúng */

/* Đèn nền: độ sáng 0..100 %. TIM4 đã được MX_TIM4_Init cấu hình PWM 500 Hz,
 * cực tính HIGH (compare = thời gian mức cao). Panel tích cực mức thấp nên
 * duty mức cao = 100 % - độ sáng. */
static void lcd_backlight_set(uint32_t percent)
{
  uint32_t period = LL_TIM_GetAutoReload(TIM4) + 1U;
  uint32_t high_percent;

  if (percent > 100U)
  {
    percent = 100U;
  }
#if (APP_LCD_BACKLIGHT_ACTIVE_LOW != 0U)
  high_percent = 100U - percent;
#else
  high_percent = percent;
#endif
  LL_TIM_OC_SetCompareCH2(TIM4, (period * high_percent) / 100U);
  LL_TIM_CC_EnableChannel(TIM4, LL_TIM_CHANNEL_CH2);
  LL_TIM_GenerateEvent_UPDATE(TIM4);   /* nạp ngay giá trị compare (preload) */
  LL_TIM_EnableCounter(TIM4);
}

/* Ghi/đọc thử vài mẫu ở cả 2 framebuffer */
static uint32_t lcd_sdram_check(void)
{
  static const uint32_t pat[] = { 0x55AA55AAUL, 0xAA55AA55UL, 0x12345678UL, 0x00000000UL };
  volatile uint32_t *p0 = (volatile uint32_t *)APP_LCD_FB0_ADDR;
  volatile uint32_t *p1 = (volatile uint32_t *)APP_LCD_FB1_ADDR;
  uint32_t i;

  for (i = 0U; i < (sizeof(pat) / sizeof(pat[0])); i++)
  {
    p0[i * 1024U] = pat[i];
    p1[i * 1024U] = ~pat[i];
  }
  /* SDRAM được cache: đẩy dữ liệu xuống chip và bỏ bản trong cache,
     để lần đọc dưới đây đọc thật từ SDRAM */
  SCB_CleanInvalidateDCache();
  for (i = 0U; i < (sizeof(pat) / sizeof(pat[0])); i++)
  {
    if ((p0[i * 1024U] != pat[i]) || (p1[i * 1024U] != ~pat[i]))
    {
      return 0U;
    }
  }
  return 1U;
}

/* ---------------------------------------------------------------------------
 * DMA2D (Chrom-ART): tô vùng nhớ bằng phần cứng, nhanh hơn vòng lặp CPU và không
 * chiếm CPU. Lập trình thanh ghi trực tiếp (không cần stm32f7xx_hal_dma2d.c).
 * ------------------------------------------------------------------------- */
#define LCD_DMA2D_MODE_R2M      (3UL << DMA2D_CR_MODE_Pos)   /* register-to-memory */
#define LCD_DMA2D_CM_RGB565     2UL
#define LCD_DMA2D_TIMEOUT_MS    50U

volatile uint32_t g_lcd_dma2d_err;   /* số lần DMA2D lỗi (đã tô lại bằng CPU) */

static void lcd_fill_cpu(uint16_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
  uint32_t r, i;

  for (r = 0U; r < h; r++)
  {
    uint16_t *p = fb + ((y + r) * APP_LCD_WIDTH) + x;
    for (i = 0U; i < w; i++) { p[i] = c; }
  }
  App_LCD_Flush();
}

void App_LCD_FillRect(uint16_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t rgb565)
{
  uint32_t t0;

  if ((x >= APP_LCD_WIDTH) || (y >= APP_LCD_HEIGHT) || (w == 0U) || (h == 0U))
  {
    return;
  }
  if ((x + w) > APP_LCD_WIDTH)  { w = APP_LCD_WIDTH - x; }
  if ((y + h) > APP_LCD_HEIGHT) { h = APP_LCD_HEIGHT - y; }

  SET_BIT(RCC->AHB1ENR, RCC_AHB1ENR_DMA2DEN);
  (void)READ_BIT(RCC->AHB1ENR, RCC_AHB1ENR_DMA2DEN);

  /* DMA2D ghi thẳng SDRAM: đẩy dữ liệu bẩn trong D-cache xuống và bỏ các dòng cache
   * cũ, nếu không CPU có thể ghi đè dữ liệu cũ lên vùng vừa tô (theo set/way, ~µs). */
  SCB_CleanInvalidateDCache();

  DMA2D->CR     = LCD_DMA2D_MODE_R2M;
  DMA2D->OPFCCR = LCD_DMA2D_CM_RGB565;
  DMA2D->OCOLR  = rgb565;
  DMA2D->OMAR   = (uint32_t)(fb + (y * APP_LCD_WIDTH) + x);
  DMA2D->OOR    = APP_LCD_WIDTH - w;
  DMA2D->NLR    = (w << DMA2D_NLR_PL_Pos) | h;
  DMA2D->IFCR   = DMA2D_IFCR_CTCIF | DMA2D_IFCR_CTEIF | DMA2D_IFCR_CCEIF;
  DMA2D->CR    |= DMA2D_CR_START;

  t0 = HAL_GetTick();
  while ((DMA2D->CR & DMA2D_CR_START) != 0U)
  {
    if ((HAL_GetTick() - t0) > LCD_DMA2D_TIMEOUT_MS)
    {
      break;
    }
  }
  if (((DMA2D->CR & DMA2D_CR_START) != 0U) ||
      ((DMA2D->ISR & (DMA2D_ISR_TEIF | DMA2D_ISR_CEIF)) != 0U))
  {
    /* Lỗi cấu hình/truy cập: dừng DMA2D, tô bằng CPU để không mất hình */
    DMA2D->CR |= DMA2D_CR_ABORT;
    DMA2D->IFCR = DMA2D_IFCR_CTCIF | DMA2D_IFCR_CTEIF | DMA2D_IFCR_CCEIF;
    g_lcd_dma2d_err++;
    lcd_fill_cpu(fb, x, y, w, h, rgb565);
    return;
  }
  DMA2D->IFCR = DMA2D_IFCR_CTCIF;
}

void App_LCD_Clear(uint16_t *fb, uint16_t rgb565)
{
  App_LCD_FillRect(fb, 0U, 0U, APP_LCD_WIDTH, APP_LCD_HEIGHT, rgb565);
}

/* SDRAM là Write-Back cacheable: dữ liệu vừa vẽ có thể còn nằm trong D-cache
 * (16 KB). Clean toàn bộ D-cache (theo set/way, ~512 dòng) rẻ hơn nhiều so với
 * clean theo địa chỉ 1.2 MB framebuffer. */
void App_LCD_Flush(void)
{
  SCB_CleanDCache();
}

void App_LCD_Init(void)
{
  lcd_ready = 0U;
  lcd_front_addr = APP_LCD_FB0_ADDR;

  /* 1. Panel và đèn nền */
  LL_GPIO_SetOutputPin(LTDC_TFT_EN_GPIO_Port, LTDC_TFT_EN_Pin);   /* DISP = 1 */
  lcd_backlight_set(APP_LCD_BACKLIGHT_PERCENT);

  /* 2. SDRAM */
  g_lcd_sdram_ok = 0U;
  if (App_SDRAM_Init() == 0U)
  {
    g_lcd_sdram_ok = lcd_sdram_check();
  }

  if (g_lcd_sdram_ok == 0U)
  {
    /* Không có framebuffer: tắt layer, hiện màu nền đỏ để báo lỗi SDRAM */
    __HAL_LTDC_LAYER_DISABLE(&hltdc, 0);
    __HAL_LTDC_LAYER_DISABLE(&hltdc, 1);
    LTDC->BCCR = 0x00FF0000UL;
    __HAL_LTDC_RELOAD_IMMEDIATE_CONFIG(&hltdc);
    return;
  }

  /* SDRAM dùng được: cho malloc dùng phần SDRAM sau 2 framebuffer */
  App_SDRAM_EnableHeap();

  /* 3. Xoá cả 2 buffer, hiển thị FB0 */
  App_LCD_Clear((uint16_t *)APP_LCD_FB0_ADDR, 0x0000U);
  App_LCD_Clear((uint16_t *)APP_LCD_FB1_ADDR, 0x0000U);
  (void)HAL_LTDC_SetAddress(&hltdc, lcd_front_addr, 0);
  lcd_ready = 1U;
  lcd_log_row = 0U;

  App_LCD_Log(LCD_COLOR_CYAN,  "IPAD STM32F767 - DEBUG");
  App_LCD_Log(LCD_COLOR_GREEN, "LCD INIT OK  (1024x600 RGB565)");
  App_LCD_Log(LCD_COLOR_GREEN, "SDRAM INIT OK (0xC0000000, 8 MB, cached WB, heap %lu KB)",
              (unsigned long)((APP_SDRAM_HEAP_END - APP_SDRAM_HEAP_START) / 1024UL));
}

uint8_t App_LCD_IsReady(void)
{
  return lcd_ready;
}

uint16_t *App_LCD_GetBackBuffer(void)
{
  return (uint16_t *)((lcd_front_addr == APP_LCD_FB0_ADDR) ? APP_LCD_FB1_ADDR : APP_LCD_FB0_ADDR);
}

void App_LCD_SwapBuffer(void)
{
  uint32_t back = (uint32_t)App_LCD_GetBackBuffer();

  App_LCD_Flush();                   /* ảnh trong back buffer phải nằm hết trong SDRAM */
  (void)HAL_LTDC_SetAddress_NoReload(&hltdc, back, 0);
  (void)HAL_LTDC_Reload(&hltdc, LTDC_RELOAD_VERTICAL_BLANKING);

  /* Chờ LTDC thực sự chuyển sang buffer mới (tối đa ~1 khung hình) trước khi
     cho phép vẽ vào buffer cũ */
  {
    uint32_t t0 = HAL_GetTick();
    while (((LTDC->SRCR & LTDC_SRCR_VBR) != 0U) && ((HAL_GetTick() - t0) < 50U)) { }
  }
  lcd_front_addr = back;
  lcd_log_row = 0U;                  /* buffer mới: log ghi lại từ dòng đầu */
}

/* ---------------------------------------------------------------------------
 * Vẽ chữ (font 8x16, phóng to 'scale' lần)
 * ------------------------------------------------------------------------- */
static void lcd_draw_char(uint16_t *fb, uint32_t x, uint32_t y, char c,
                          uint16_t fg, uint16_t bg, uint32_t scale)
{
  const uint8_t *glyph;
  uint32_t row, col, sx, sy;

  if (((uint8_t)c < APP_FONT_FIRST) || ((uint8_t)c > APP_FONT_LAST))
  {
    c = '?';
  }
  glyph = g_font8x16[(uint8_t)c - APP_FONT_FIRST];

  for (row = 0U; row < APP_FONT_H; row++)
  {
    for (sy = 0U; sy < scale; sy++)
    {
      uint32_t py = y + (row * scale) + sy;
      uint16_t *line;

      if (py >= APP_LCD_HEIGHT)
      {
        return;
      }
      line = fb + (py * APP_LCD_WIDTH);
      for (col = 0U; col < APP_FONT_W; col++)
      {
        uint16_t color = ((glyph[row] & (0x80U >> col)) != 0U) ? fg : bg;

        for (sx = 0U; sx < scale; sx++)
        {
          uint32_t px = x + (col * scale) + sx;

          if (px < APP_LCD_WIDTH)
          {
            line[px] = color;
          }
        }
      }
    }
  }
}

void App_LCD_DrawText(uint16_t *fb, uint32_t x, uint32_t y, const char *s,
                      uint16_t fg, uint16_t bg, uint32_t scale)
{
  while ((*s != '\0') && (x < APP_LCD_WIDTH))
  {
    lcd_draw_char(fb, x, y, *s, fg, bg, scale);
    x += APP_FONT_W * scale;
    s++;
  }
}

/* ---------------------------------------------------------------------------
 * Log debug lên buffer đang hiển thị
 * ------------------------------------------------------------------------- */
#define LCD_LOG_MARGIN   8U
#define LCD_LOG_LINE_H   (APP_FONT_H * APP_LCD_LOG_SCALE + 4U)
#define LCD_LOG_ROWS     ((APP_LCD_HEIGHT - LCD_LOG_MARGIN) / LCD_LOG_LINE_H)
#define LCD_LOG_COLS     ((APP_LCD_WIDTH - 2U * LCD_LOG_MARGIN) / (APP_FONT_W * APP_LCD_LOG_SCALE))

void App_LCD_LogClear(void)
{
  if (lcd_ready == 0U)
  {
    return;
  }
  App_LCD_Clear((uint16_t *)lcd_front_addr, LCD_COLOR_BLACK);
  lcd_log_row = 0U;
}

void App_LCD_Log(uint16_t color, const char *fmt, ...)
{
  char line[LCD_LOG_COLS + 1U];
  va_list ap;

  if (lcd_ready == 0U)
  {
    return;
  }
#if (APP_LCD_LOG_ERRORS_ONLY != 0U)
  if ((color != LCD_COLOR_RED) && (color != LCD_COLOR_YELLOW))
  {
    return;                          /* bỏ log thông tin, chỉ giữ lỗi/cảnh báo */
  }
#endif
  if (lcd_log_row >= LCD_LOG_ROWS)
  {
    App_LCD_LogClear();
  }

  va_start(ap, fmt);
  (void)vsnprintf(line, sizeof(line), fmt, ap);   /* dòng dài bị cắt bớt */
  va_end(ap);

  App_LCD_DrawText((uint16_t *)lcd_front_addr, LCD_LOG_MARGIN,
                   LCD_LOG_MARGIN + (lcd_log_row * LCD_LOG_LINE_H),
                   line, color, LCD_COLOR_BLACK, APP_LCD_LOG_SCALE);
  App_LCD_Flush();                   /* vẽ vào buffer đang hiển thị: cho LTDC thấy ngay */
  lcd_log_row++;
}
