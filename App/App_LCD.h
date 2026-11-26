/*
 * App_LCD.h
 *
 * App LCD: khởi tạo panel, đèn nền và 2 framebuffer RGB565 1024x600 ở SDRAM
 * (double buffer). App khác vẽ vào back buffer rồi gọi App_LCD_SwapBuffer().
 */

#ifndef APP_APP_LCD_H_
#define APP_APP_LCD_H_

#include <stdint.h>

#define APP_LCD_WIDTH        1024U
#define APP_LCD_HEIGHT       600U
#define APP_LCD_FB0_ADDR     0xC0000000UL     /* phải khớp FBStartAdress của layer 0 */
#define APP_LCD_FB1_ADDR     0xC0140000UL     /* buffer thứ 2 (FB0 dài 1.2 MB = 0x12C000) */
/* Từ 0xC0280000 trở đi: buffer JPEG + heap mở rộng (xem App_SDRAM.h) */

/* SDRAM được cache (Write-Back): sau khi vẽ thẳng vào buffer ĐANG hiển thị phải gọi
 * App_LCD_Flush() để LTDC thấy. App_LCD_SwapBuffer/Clear/Log đã tự gọi. */

/* Đèn nền: PD13 (TIM4_CH2, PWM 500 Hz) nối thẳng chân 35 của FFC panel.
 * Đã đo thực tế: chân này tích cực mức thấp, duty mức cao càng lớn thì càng TỐI.
 * APP_LCD_BACKLIGHT_PERCENT: độ sáng 0..100 % (duty mức cao trên PD13 = 100 - giá trị này).
 * Hiện tại 80 % -> duty mức cao 20 %. */
#define APP_LCD_BACKLIGHT_PERCENT     80U
#define APP_LCD_BACKLIGHT_ACTIVE_LOW  1U

void      App_LCD_Init(void);
uint8_t   App_LCD_IsReady(void);            /* 1 nếu SDRAM/framebuffer dùng được */
uint16_t *App_LCD_GetBackBuffer(void);      /* buffer đang KHÔNG hiển thị, để vẽ */
void      App_LCD_SwapBuffer(void);         /* hiển thị back buffer (đổi ở vertical blanking) */
void      App_LCD_Flush(void);              /* ghi D-cache xuống SDRAM cho LTDC đọc */
void      App_LCD_Clear(uint16_t *fb, uint16_t rgb565);          /* tô cả buffer (DMA2D) */
/* Tô hình chữ nhật bằng DMA2D (Chrom-ART, chế độ register-to-memory), chờ xong mới trả về.
 * Tự clean + invalidate D-cache trước khi DMA2D ghi vào SDRAM. */
void      App_LCD_FillRect(uint16_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                           uint16_t rgb565);

/* ---- Chữ và log debug trên màn hình ----
 * Font 8x16 phóng to APP_LCD_LOG_SCALE lần. Log ghi vào buffer đang hiển thị,
 * mỗi lần gọi là một dòng; đầy màn hình thì xoá và ghi lại từ đầu. */
#define APP_LCD_LOG_SCALE   2U
/* 1 = chỉ in log lỗi/cảnh báo (màu đỏ, vàng); log thông tin lúc khởi động bị bỏ.
 * 0 = in tất cả (dùng khi cần debug). */
#define APP_LCD_LOG_ERRORS_ONLY  1U
#define LCD_COLOR_BLACK     0x0000U
#define LCD_COLOR_WHITE     0xFFFFU
#define LCD_COLOR_RED       0xF800U
#define LCD_COLOR_GREEN     0x07E0U
#define LCD_COLOR_YELLOW    0xFFE0U
#define LCD_COLOR_CYAN      0x07FFU

void      App_LCD_DrawText(uint16_t *fb, uint32_t x, uint32_t y, const char *s,
                           uint16_t fg, uint16_t bg, uint32_t scale);
void      App_LCD_LogClear(void);
void      App_LCD_Log(uint16_t color, const char *fmt, ...);

#endif /* APP_APP_LCD_H_ */
