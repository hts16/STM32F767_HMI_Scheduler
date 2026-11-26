/*
 * App_SDCARD.h
 *
 * App SDCARD: mount thẻ SD (FatFs), tìm ảnh .jpg/.jpeg ở thư mục gốc,
 * giải mã bằng LibJPEG và hiển thị lên LCD. Ảnh đầu tiên giữ cố định;
 * mỗi lần chạm màn hình (App_TOUCH) thì chuyển sang ảnh kế tiếp.
 */

#ifndef APP_APP_SDCARD_H_
#define APP_APP_SDCARD_H_

#include <stdint.h>

#define APP_SDCARD_TASK_MS      20U     /* chu kỳ task: kiểm tra chạm */
#define APP_SDCARD_RETRY_MS     3000U   /* chưa có thẻ/ảnh: thử mount lại sau mỗi khoảng này */
#define APP_SDCARD_MAX_FILES    64U     /* số ảnh tối đa được liệt kê */

#define APP_SDCARD_PATH_LEN     96U     /* độ dài tối đa đường dẫn một ảnh */

/* Đo tốc độ đọc thẻ lúc mount (in lên log, xem g_sd_bench_kBps). 0 = tắt (đã test xong). */
#define APP_SDCARD_BENCH_KB     0U

/* In 1-2 dòng thống kê nhỏ ở góc dưới mỗi ảnh: đường giải mã (HW JPEG / LibJPEG),
 * kích thước, thời gian đọc thẻ / giải mã. 0 = tắt. */
#define APP_SDCARD_SHOW_STATS   1U

/* Tự kiểm tra 1 lần với ảnh đầu tiên giải mã bằng phần cứng: giải mã lại bằng LibJPEG
 * (độ phân giải đầy đủ, cùng cách lấy mẫu) vào buffer tạm rồi so từng điểm ảnh.
 * Kết quả in ở dòng thống kê thứ 2. 0 = tắt. */
#define APP_SDCARD_JPEG_SELFTEST 1U

/* Mã trạng thái, xem bằng debugger qua g_sd_status */
typedef enum
{
  SD_APP_OK = 0,
  SD_APP_NO_CARD,          /* không phát hiện thẻ (PD4) hoặc mount lỗi */
  SD_APP_NO_IMAGE,         /* thẻ không có file .jpg/.jpeg ở thư mục gốc */
  SD_APP_NO_LCD,           /* framebuffer chưa sẵn sàng (lỗi SDRAM) */
} sd_app_status_t;

void App_SDCARD_Init(void);
void App_SDCARD_Task(void *pvParam);

/* Ngắt cho đọc thẻ bằng DMA, gọi từ stm32f7xx_it.c */
void App_SDCARD_SDMMC_IRQHandler(void);     /* SDMMC1_IRQHandler */
void App_SDCARD_DMA_RX_IRQHandler(void);    /* DMA2_Stream3_IRQHandler */

#endif /* APP_APP_SDCARD_H_ */
