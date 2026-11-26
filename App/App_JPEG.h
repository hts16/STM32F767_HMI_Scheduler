/*
 * App_JPEG.h
 *
 * Giải mã JPEG bằng bộ codec phần cứng của STM32F767 (JPEG, AHB2), lập trình
 * trực tiếp thanh ghi (không cần stm32f7xx_hal_jpeg.c).
 *
 *  - Phần cứng tự đọc header (HDR = 1), giải mã Huffman/IDCT và trả ra các khối
 *    MCU YCbCr 8x8 qua FIFO ra.
 *  - CPU lấy từng MCU, chỉ chuyển màu YCbCr -> RGB565 cho những điểm ảnh thực sự
 *    được hiển thị (thu nhỏ/phóng to nearest-neighbor vừa 1024x600, giữ tỉ lệ),
 *    ghi thẳng vào framebuffer.
 *  - Dữ liệu vào được nạp theo khối qua hàm đọc (đọc file từ thẻ SD vào SDRAM),
 *    nên không giới hạn kích thước file.
 *
 * Hỗ trợ: baseline / extended sequential 8-bit (SOF0/SOF1), grayscale hoặc YCbCr
 * 4:4:4, 4:2:2, 4:4:0, 4:2:0. Không hỗ trợ (trả APP_JPEG_UNSUPPORTED để dùng LibJPEG):
 * progressive (SOF2), lossless, arithmetic, 12-bit, CMYK.
 */

#ifndef APP_APP_JPEG_H_
#define APP_APP_JPEG_H_

#include <stdint.h>

/* Mã trả về của App_JPEG_DecodeHW */
#define APP_JPEG_OK            0U
#define APP_JPEG_UNSUPPORTED   1U    /* định dạng phần cứng không giải mã được -> dùng LibJPEG */
#define APP_JPEG_TIMEOUT       2U    /* codec không tiến triển (file hỏng / thiếu dữ liệu) */
#define APP_JPEG_READ_ERR      3U    /* hàm đọc dữ liệu báo lỗi */

/* Không có dữ liệu vào/ra trong khoảng này thì coi là treo */
#define APP_JPEG_STALL_MS      500U

/* Khung hiển thị: ảnh src_w x src_h được đặt vừa vào (x0, y0, w, h) trên màn hình */
typedef struct
{
  uint32_t src_w;
  uint32_t src_h;
  uint32_t x0;
  uint32_t y0;
  uint32_t w;
  uint32_t h;
} app_jpeg_fit_t;

/* Thông tin sau khi giải mã */
typedef struct
{
  uint32_t width;
  uint32_t height;
  uint32_t ncomp;          /* 1 = grayscale, 3 = YCbCr */
  uint32_t hsf;            /* hệ số lấy mẫu ngang của Y (1/2) */
  uint32_t vsf;            /* hệ số lấy mẫu dọc của Y (1/2) */
  uint32_t mcus;           /* số MCU đã nhận */
  uint32_t in_bytes;       /* số byte đã nạp vào codec */
  uint32_t ms;             /* thời gian giải mã (không tính thời gian đọc file) */
  uint32_t read_ms;        /* thời gian các lần gọi hàm đọc trong lúc giải mã */
} app_jpeg_info_t;

/* Hàm đọc dữ liệu vào: ghi tối đa 'size' byte vào buf, trả về số byte đã đọc
 * (0 = hết file), hoặc 0xFFFFFFFF nếu lỗi. */
typedef uint32_t (*app_jpeg_read_t)(void *ctx, uint8_t *buf, uint32_t size);

extern volatile uint32_t g_jpeg_hw_sr;   /* JPEG->SR lúc codec bị treo (debug) */

/* Tính khung hiển thị vừa màn hình, giữ tỉ lệ, căn giữa */
void     App_JPEG_Fit(uint32_t src_w, uint32_t src_h, app_jpeg_fit_t *fit);

/* Đọc các marker đầu file.
 * Trả về 1: phần cứng giải mã được, 0: không hỗ trợ (progressive...), -1: không tìm thấy SOF.
 * *w, *h: kích thước ảnh (nếu tìm thấy SOF). *sof: mã marker SOF (0xC0, 0xC2...). */
int32_t  App_JPEG_Probe(const uint8_t *data, uint32_t len, uint32_t *w, uint32_t *h, uint32_t *sof);

/* Giải mã bằng phần cứng vào framebuffer fb theo khung 'fit'.
 * buf chứa sẵn 'len' byte đầu file; khi dùng hết, gọi rd(ctx, buf, buf_size) để nạp tiếp.
 * buf phải căn 4 byte, buf_size bội số 4. Phần viền ngoài khung không bị ghi. */
uint32_t App_JPEG_DecodeHW(uint8_t *buf, uint32_t len, uint32_t buf_size,
                           app_jpeg_read_t rd, void *ctx,
                           uint16_t *fb, const app_jpeg_fit_t *fit, app_jpeg_info_t *info);

/* ---- Phần chuyển MCU -> RGB565 (tách riêng để kiểm thử trên PC) ---- */
typedef struct
{
  uint16_t *fb;            /* framebuffer APP_LCD_WIDTH x APP_LCD_HEIGHT */
  uint32_t  x0, y0, fw, fh;
  uint32_t  w, h;          /* kích thước ảnh gốc */
  uint32_t  ncomp, hsf, vsf, ny;
  uint32_t  mcu_w, mcu_h, mcu_bytes, mcols;
  uint32_t  mcx, mcy;      /* vị trí MCU kế tiếp */
} app_jpeg_core_t;

/* Trả về 1 nếu cấu hình hợp lệ */
uint32_t App_JPEG_CoreSetup(app_jpeg_core_t *c, uint16_t *fb, const app_jpeg_fit_t *fit,
                            uint32_t w, uint32_t h, uint32_t ncomp, uint32_t hsf, uint32_t vsf);
/* Đưa một MCU (c->mcu_bytes byte, thứ tự khối như codec xuất ra) vào framebuffer */
void     App_JPEG_CorePutMCU(app_jpeg_core_t *c, const uint8_t *mcu);

#endif /* APP_APP_JPEG_H_ */
