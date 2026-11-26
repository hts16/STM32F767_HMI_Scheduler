/*
 * App_SDRAM.h
 *
 * App SDRAM: chạy chuỗi lệnh khởi tạo chip IS42S16400 (sau khi FMC đã cấu hình
 * bằng MX_FMC_Init). SDRAM bank 1 ở 0xC0000000, 8 MB.
 */

#ifndef APP_APP_SDRAM_H_
#define APP_APP_SDRAM_H_

#include <stdint.h>

#define APP_SDRAM_BASE_ADDR   0xC0000000UL
#define APP_SDRAM_SIZE_BYTES  (8UL * 1024UL * 1024UL)

/* Bản đồ SDRAM (toàn bộ 8 MB là Normal Write-Back cacheable, xem MPU_Config_FMC):
 *   0xC0000000  FB0 (1.2 MB)            - App_LCD.h
 *   0xC0140000  FB1 (1.2 MB)
 *   0xC0280000  buffer đọc file JPEG    - App_SDCARD (DMA thẻ SD ghi vào đây)
 *   0xC0380000  heap mở rộng cho malloc (LibJPEG khi RAM trong hết)
 * Địa chỉ/kích thước buffer phải bội số 32 byte (dòng cache) vì DMA ghi vào. */
#define APP_SDRAM_JPEG_BUF_ADDR  0xC0280000UL
#define APP_SDRAM_JPEG_BUF_SIZE  (1024UL * 1024UL)   /* 1 MB: đa số ảnh đọc 1 lần f_read */

#define APP_SDRAM_HEAP_START  (APP_SDRAM_JPEG_BUF_ADDR + APP_SDRAM_JPEG_BUF_SIZE)
#define APP_SDRAM_HEAP_END    (APP_SDRAM_BASE_ADDR + APP_SDRAM_SIZE_BYTES)   /* 4.5 MB */

/* Trả về 0 nếu thành công, khác 0 nếu lỗi gửi lệnh */
uint32_t App_SDRAM_Init(void);

/* Cho malloc dùng thêm SDRAM khi RAM trong hết (gọi sau khi SDRAM đã kiểm tra OK) */
void App_SDRAM_EnableHeap(void);

#endif /* APP_APP_SDRAM_H_ */
