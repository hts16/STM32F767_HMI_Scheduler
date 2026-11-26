/*
 * App_SDCARD.c
 *
 * Trình chiếu ảnh JPEG từ thẻ SD:
 *  - Mount thẻ bằng FatFs (driver SD do CubeMX sinh: FATFS/Target/sd_diskio.c).
 *  - Liệt kê file .jpg/.jpeg ở thư mục gốc (tối đa APP_SDCARD_MAX_FILES).
 *  - Giải mã bằng LibJPEG vào back buffer RGB565, thu nhỏ/phóng to cho vừa
 *    1024x600 và giữ đúng tỉ lệ, căn giữa trên nền đen, rồi đổi buffer.
 *  - Ảnh đầu tiên giữ cố định. Scheduler gọi App_SDCARD_Task mỗi 20 ms; khi
 *    App_TOUCH báo có lần chạm mới thì chuyển sang ảnh kế tiếp.
 *  - Ngay sau khi hiện một ảnh, ảnh kế tiếp được giải mã sẵn vào back buffer,
 *    nên khi chạm chỉ cần đổi buffer (gần như tức thì).
 *    Nếu chưa có thẻ/ảnh, task thử mount lại mỗi APP_SDCARD_RETRY_MS (3 s).
 *
 * Mỗi bước và mỗi lỗi được in lên màn hình (App_LCD_Log). Khi ảnh hiển thị
 * thành công thì chỉ còn ảnh trên màn hình.
 *
 * Tốc độ:
 *  - Thẻ đọc bằng DMA (DMA2 Stream3) ở 4-bit High Speed 50 MHz, không còn
 *    RX overrun như khi CPU tự rút FIFO.
 *  - File JPEG được đọc từng khối lớn (APP_SDRAM_JPEG_BUF_SIZE) thẳng vào SDRAM
 *    thay vì 4 KB/lần như jpeg_stdio_src -> FatFs gửi lệnh đọc nhiều block liên tiếp.
 *  - SDRAM được cache (Write-Back) nên giải mã/vẽ vào framebuffer nhanh hơn.
 *
 * Lưu ý: giải mã chạy trong task (blocking), ảnh lớn có thể mất vài trăm ms.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#include "App_SDCARD.h"
#include "App_LCD.h"
#include "App_TOUCH.h"
#include "App_SDRAM.h"
#include "main.h"
#include "fatfs.h"
#include "bsp_driver_sd.h"
#include "jpeglib.h"
#include "jerror.h"
#include "App_JPEG.h"

extern SD_HandleTypeDef hsd1;

/* ---------------------------------------------------------------------------
 * Trạng thái để xem bằng debugger (Live Expressions)
 * ------------------------------------------------------------------------- */
volatile sd_app_status_t g_sd_status = SD_APP_NO_CARD;
volatile uint32_t        g_sd_jpg_count;     /* số ảnh tìm thấy */
volatile uint32_t        g_sd_jpg_index;     /* ảnh đang hiển thị */
volatile uint32_t        g_sd_fail_count;    /* số ảnh giải mã lỗi */
volatile uint32_t        g_sd_last_ms;       /* thời gian giải mã ảnh gần nhất (ms) */
volatile FRESULT         g_sd_last_fresult;  /* kết quả FatFs gần nhất */
volatile uint32_t        g_sd_bus_width;     /* 4 hoặc 1 (bit) sau khi khởi tạo */
volatile uint32_t        g_sd_err_4bit;      /* mã lỗi HAL khi thử 4-bit (0 = không lỗi) */

/* ---------------------------------------------------------------------------
 * Biến nội bộ (static để không chiếm stack)
 * ------------------------------------------------------------------------- */
static char     sd_files[APP_SDCARD_MAX_FILES][APP_SDCARD_PATH_LEN];
static uint32_t sd_count;
static uint32_t sd_next;
static uint8_t  sd_mounted;
static uint8_t  sd_shown_once;                 /* đã hiện được ít nhất một ảnh */
static uint32_t sd_retry_ms;                   /* thời điểm thử mount gần nhất */
static uint32_t sd_back_idx;                   /* ảnh đang nằm trong back buffer */
static uint8_t  sd_back_ready;                 /* 1 = back buffer đã có ảnh kế tiếp */
static uint8_t  sd_need_preload;               /* 1 = cần giải mã sẵn ảnh kế tiếp */
static sd_app_status_t sd_logged_status = (sd_app_status_t)0xFF;   /* tránh in lặp */

static FIL      sd_jpg_file;
static struct jpeg_decompress_struct sd_cinfo;

typedef struct
{
  struct jpeg_error_mgr pub;   /* phải là phần tử đầu tiên */
  jmp_buf               jump;
} sd_jpeg_err_t;

static sd_jpeg_err_t sd_jerr;
static char          sd_jpeg_msg[JMSG_LENGTH_MAX];
static uint32_t      sd_img_w, sd_img_h;        /* kích thước ảnh gốc (0 = chưa đọc header) */
static uint8_t       sd_img_progressive;

/* ---------------------------------------------------------------------------
 * Khởi tạo thẻ SD (thay thế BSP_SD_Init / BSP_SD_ReadBlocks __weak trong
 * FATFS/Target/bsp_driver_sd.c, nên không mất khi CubeMX generate lại).
 *
 * CỐ ĐỊNH: bus 4-bit, SDMMC_CK = 50 MHz / (0 + 2) = 25 MHz (default speed, không
 * CMD6/High Speed), đọc bằng DMA vào bộ nhớ.
 *  - Đã đo: 25 MHz 4-bit đạt; 50 MHz cần hạ sườn CLK PC12 (đã đặt HIGH trong .ioc).
 *  - Khởi tạo: HAL nhận dạng thẻ ở 400 kHz, chạy 1-bit 25 MHz, chuyển 4-bit (ACMD6),
 *    rồi đọc thử SD_VERIFY_BLOCKS x SD_VERIFY_ROUNDS sector để xác nhận.
 *  - Không tự hạ tốc độ: lỗi khởi tạo hoặc lỗi đọc đều in rõ lên màn hình.
 * ------------------------------------------------------------------------- */
#define SD_RUN_CLKDIV           0U        /* 25 MHz, khớp SDMMC1.ClockDiv = 0 trong .ioc */
#define SD_VERIFY_BLOCKS        16U       /* số sector đọc thử khi khởi tạo */
#define SD_VERIFY_ROUNDS        4U
#define SD_READY_TIMEOUT_MS     500U
#define SD_IO_TIMEOUT_MS        1000U
#define SD_SDMMCCLK_KHZ         50000U    /* SDMMCCLK = PLL48 (PLLQ = 8) = 50 MHz */

/* Căn 32 byte (dòng cache) vì DMA ghi vào; cũng là buffer trung gian cho buffer không căn */
static __attribute__((aligned(32))) uint8_t sd_sector_buf[512U * SD_VERIFY_BLOCKS];
volatile uint32_t g_sd_clkdiv;            /* ClockDiv đang dùng */
volatile uint32_t g_sd_clk_khz;           /* SDMMC_CK đang dùng (kHz) */
volatile uint8_t  g_sd_high_speed;        /* luôn 0: không dùng High Speed */
volatile uint32_t g_sd_read_retry;        /* số lần đọc lỗi phải đọc lại */
volatile uint32_t g_sd_last_read_err;     /* hsd1.ErrorCode lần đọc lỗi gần nhất */
volatile uint32_t g_sd_verify_err;        /* ErrorCode của lần đọc thử lỗi gần nhất */
volatile uint32_t g_sd_init_step;         /* bước khởi tạo bị lỗi: 1 HAL_SD_Init, 2 4-bit, 3 đọc thử */
volatile uint32_t g_sd_rt_err;            /* số lần đọc lỗi khi chạy */

static uint8_t sd_wait_transfer(uint32_t timeout_ms)
{
  uint32_t t0 = HAL_GetTick();

  while ((HAL_GetTick() - t0) < timeout_ms)
  {
    if (HAL_SD_GetCardState(&hsd1) == HAL_SD_CARD_TRANSFER)
    {
      return 1U;
    }
  }
  return 0U;
}

/* Sau một lần đọc lỗi: dừng DPSM, xoá cờ, gửi CMD12 để thẻ thoát trạng thái gửi dữ liệu */
static void sd_recover(void)
{
  SDMMC1->DCTRL = 0U;
  __SDMMC_CLEAR_FLAG(SDMMC1, SDMMC_STATIC_FLAGS);
  (void)SDMMC_CmdStopTransfer(SDMMC1);
  __SDMMC_CLEAR_FLAG(SDMMC1, SDMMC_STATIC_FLAGS);
  (void)sd_wait_transfer(SD_READY_TIMEOUT_MS);
  hsd1.ErrorCode = HAL_SD_ERROR_NONE;
  hsd1.State = HAL_SD_STATE_READY;
}

/* ---------------------------------------------------------------------------
 * Đọc thẻ bằng DMA (DMA2 Stream3 Channel 4 = SDMMC1_RX)
 *
 * Đọc polling (CPU tự rút FIFO 32 word) dễ bị RX overrun ở 50 MHz khi có ngắt
 * (SysTick, touch) chen vào. DMA rút FIFO bằng phần cứng nên chạy ổn định ở
 * 4-bit 50 MHz - mức tối đa SDMMC của STM32F7 hỗ trợ (High Speed / SDR25,
 * ~23 MB/s lý thuyết; UHS-I cần 1.8 V nên không dùng được).
 *
 * DMA ghi thẳng vào bộ nhớ, không qua D-cache: buffer phải căn 32 byte và được
 * invalidate trước/sau khi đọc. Buffer không căn (sector buffer trong FATFS/FIL)
 * thì đọc qua sd_sector_buf rồi chép.
 * Ghi thẻ vẫn dùng polling (BSP_SD_WriteBlocks mặc định), app chỉ đọc.
 * ------------------------------------------------------------------------- */
#define SD_DMA_MAX_BLOCKS       256U      /* tối đa mỗi lần DMA: 128 KB (NDTR 16-bit: 32768 word) */

static DMA_HandleTypeDef sd_hdma_rx;
static DMA_HandleTypeDef sd_hdma_tx_dummy;   /* HAL đọc hdmatx->ErrorCode khi DMA lỗi, không được NULL */
static volatile uint8_t  sd_dma_state;       /* 0 = đang chạy, 1 = xong, 2 = lỗi */
static uint8_t           sd_dma_ready;       /* 1 = DMA đã khởi tạo */
volatile uint32_t g_sd_dma_err;              /* số lần đọc DMA lỗi / quá thời gian */

static void sd_dma_init(void)
{
  if (sd_dma_ready == 0U)
  {
    __HAL_RCC_DMA2_CLK_ENABLE();
    sd_hdma_rx.Instance                 = DMA2_Stream3;
    sd_hdma_rx.Init.Channel             = DMA_CHANNEL_4;
    sd_hdma_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    sd_hdma_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
    sd_hdma_rx.Init.MemInc              = DMA_MINC_ENABLE;
    sd_hdma_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    sd_hdma_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
    sd_hdma_rx.Init.Mode                = DMA_PFCTRL;            /* SDMMC báo hết dữ liệu */
    sd_hdma_rx.Init.Priority            = DMA_PRIORITY_VERY_HIGH;
    sd_hdma_rx.Init.FIFOMode            = DMA_FIFOMODE_ENABLE;
    sd_hdma_rx.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
    sd_hdma_rx.Init.MemBurst            = DMA_MBURST_INC4;
    sd_hdma_rx.Init.PeriphBurst         = DMA_PBURST_INC4;
    if (HAL_DMA_Init(&sd_hdma_rx) != HAL_OK)
    {
      return;                                  /* sd_dma_ready = 0: đọc bằng polling */
    }
    HAL_NVIC_SetPriority(SDMMC1_IRQn, 5U, 0U);
    HAL_NVIC_EnableIRQ(SDMMC1_IRQn);
    HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 6U, 0U);
    HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);
    sd_dma_ready = 1U;
  }
  __HAL_LINKDMA(&hsd1, hdmarx, sd_hdma_rx);
  __HAL_LINKDMA(&hsd1, hdmatx, sd_hdma_tx_dummy);
}

/* Gọi từ stm32f7xx_it.c */
void App_SDCARD_SDMMC_IRQHandler(void)
{
  HAL_SD_IRQHandler(&hsd1);
}

void App_SDCARD_DMA_RX_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&sd_hdma_rx);
}

/* Callback của HAL / BSP (thay bản __weak) */
void BSP_SD_ReadCpltCallback(void)
{
  if (sd_dma_state == 0U)
  {
    sd_dma_state = 1U;
  }
}

void BSP_SD_AbortCallback(void)
{
  sd_dma_state = 2U;
}

void HAL_SD_ErrorCallback(SD_HandleTypeDef *hsd)
{
  (void)hsd;
  sd_dma_state = 2U;
}

/* Dọn sau mỗi lần DMA: tắt ngắt dữ liệu của SDMMC (HAL có thể để bật), nếu không
 * các lệnh đọc polling sau đó (CMD6, đọc SCR...) bị ISR lấy mất cờ DATAEND.
 * abort = 1: dừng luôn stream DMA (HAL_SD_ReadBlocks_DMA lỗi lệnh không tự dừng). */
static void sd_dma_cleanup(uint8_t abort)
{
  __HAL_SD_DISABLE_IT(&hsd1, SDMMC_IT_DATAEND | SDMMC_IT_DCRCFAIL | SDMMC_IT_DTIMEOUT |
                             SDMMC_IT_RXOVERR | SDMMC_IT_TXUNDERR);
  if (abort != 0U)
  {
    hsd1.Instance->DCTRL &= ~SDMMC_DCTRL_DMAEN;
    (void)HAL_DMA_Abort(&sd_hdma_rx);
  }
  __HAL_SD_CLEAR_FLAG(&hsd1, SDMMC_STATIC_DATA_FLAGS);   /* không để cờ DATAEND cũ cho lệnh polling sau */
  HAL_NVIC_ClearPendingIRQ(SDMMC1_IRQn);
}

/* Đọc n block (n <= SD_DMA_MAX_BLOCKS) vào buf đã căn 32 byte. Trả về 1 nếu OK. */
static uint8_t sd_read_raw(uint8_t *buf, uint32_t blk, uint32_t n, uint32_t timeout_ms)
{
  int32_t  len = (int32_t)(n * 512U);
  uint32_t t0;

  if (sd_dma_ready == 0U)
  {
    return (HAL_SD_ReadBlocks(&hsd1, buf, blk, n, timeout_ms) == HAL_OK) ? 1U : 0U;
  }

  SCB_InvalidateDCache_by_Addr((uint32_t *)buf, len);   /* bỏ dòng cache cũ, tránh bị ghi đè lên dữ liệu DMA */
  sd_dma_state = 0U;
  if (HAL_SD_ReadBlocks_DMA(&hsd1, buf, blk, n) != HAL_OK)
  {
    sd_dma_cleanup(1U);
    g_sd_dma_err++;
    return 0U;
  }

  t0 = HAL_GetTick();
  while (sd_dma_state == 0U)
  {
    if ((HAL_GetTick() - t0) >= timeout_ms)
    {
      (void)HAL_SD_Abort(&hsd1);
      sd_dma_state = 2U;
    }
  }
  sd_dma_cleanup((sd_dma_state != 1U) ? 1U : 0U);
  SCB_InvalidateDCache_by_Addr((uint32_t *)buf, len);   /* CPU đọc dữ liệu DMA vừa ghi, không đọc cache */

  if (sd_dma_state != 1U)
  {
    g_sd_dma_err++;
    return 0U;
  }
  return 1U;
}

/* Đọc n block vào buffer bất kỳ: căn 32 byte thì DMA thẳng vào buffer,
 * không căn thì đọc qua sd_sector_buf rồi chép. Trả về 1 nếu OK. */
static uint8_t sd_read_any(uint8_t *dst, uint32_t blk, uint32_t n, uint32_t timeout_ms)
{
  while (n > 0U)
  {
    uint32_t cnt;

    if (((uint32_t)dst & 31U) == 0U)
    {
      cnt = (n > SD_DMA_MAX_BLOCKS) ? SD_DMA_MAX_BLOCKS : n;
      if (sd_read_raw(dst, blk, cnt, timeout_ms) == 0U)
      {
        return 0U;
      }
    }
    else
    {
      cnt = (n > SD_VERIFY_BLOCKS) ? SD_VERIFY_BLOCKS : n;
      if (sd_read_raw(sd_sector_buf, blk, cnt, timeout_ms) == 0U)
      {
        return 0U;
      }
      memcpy(dst, sd_sector_buf, cnt * 512U);
    }
    dst += cnt * 512U;
    blk += cnt;
    n   -= cnt;
    if ((n > 0U) && (sd_wait_transfer(SD_READY_TIMEOUT_MS) == 0U))
    {
      return 0U;
    }
  }
  return 1U;
}

/* Đổi clock SDMMC_CK khi đang chạy. bypass = 1: SDMMC_CK = SDMMCCLK (50 MHz) */
static void sd_set_clock(uint32_t clkdiv, uint8_t bypass)
{
  uint32_t clkcr = SDMMC1->CLKCR;

  clkcr &= ~(SDMMC_CLKCR_CLKDIV | SDMMC_CLKCR_BYPASS);
  clkcr |= (clkdiv & SDMMC_CLKCR_CLKDIV);
  if (bypass != 0U)
  {
    clkcr |= SDMMC_CLKCR_BYPASS;
  }
  SDMMC1->CLKCR = clkcr;

  hsd1.Init.ClockDiv    = clkdiv;
  hsd1.Init.ClockBypass = (bypass != 0U) ? SDMMC_CLOCK_BYPASS_ENABLE : SDMMC_CLOCK_BYPASS_DISABLE;
  g_sd_clkdiv  = clkdiv;
  g_sd_clk_khz = (bypass != 0U) ? SD_SDMMCCLK_KHZ : (SD_SDMMCCLK_KHZ / (clkdiv + 2U));
  HAL_Delay(1U);
}

/* Đọc thử nhiều lần ở tốc độ hiện tại. Trả về 1 nếu tất cả đều OK. */
static uint8_t sd_verify(void)
{
  uint32_t r;

  for (r = 0U; r < SD_VERIFY_ROUNDS; r++)
  {
    if (sd_read_raw(sd_sector_buf, r * SD_VERIFY_BLOCKS, SD_VERIFY_BLOCKS,
                    SD_IO_TIMEOUT_MS) == 0U)
    {
      g_sd_verify_err = hsd1.ErrorCode;
      sd_recover();
      return 0U;
    }
    if (sd_wait_transfer(SD_READY_TIMEOUT_MS) == 0U)
    {
      g_sd_verify_err = 0xEEEEEEEEU;   /* thẻ không về trạng thái TRANSFER */
      return 0U;
    }
  }
  return 1U;
}

/* Tên loại lỗi từ hsd1.ErrorCode */
static const char *sd_err_str(uint32_t err)
{
  if (err == 0U)                                  { return "SW-TIMEOUT"; }
  if ((err & HAL_SD_ERROR_RX_OVERRUN) != 0U)      { return "RX-OVERRUN"; }
  if ((err & HAL_SD_ERROR_DATA_CRC_FAIL) != 0U)   { return "DATA-CRC"; }
  if ((err & HAL_SD_ERROR_DATA_TIMEOUT) != 0U)    { return "DATA-TIMEOUT"; }
  if ((err & HAL_SD_ERROR_DMA) != 0U)             { return "DMA"; }
  if ((err & HAL_SD_ERROR_CMD_CRC_FAIL) != 0U)    { return "CMD-CRC"; }
  if ((err & HAL_SD_ERROR_CMD_RSP_TIMEOUT) != 0U) { return "CMD-TIMEOUT"; }
  return "OTHER";
}

uint8_t BSP_SD_Init(void)
{
  g_sd_bus_width  = 0U;
  g_sd_err_4bit   = 0U;
  g_sd_init_step  = 0U;
  g_sd_high_speed = 0U;

  if (BSP_SD_IsDetected() != SD_PRESENT)
  {
    return MSD_ERROR_SD_NOT_PRESENT;
  }

  (void)HAL_SD_DeInit(&hsd1);
  HAL_Delay(10U);

  hsd1.Instance                 = SDMMC1;
  hsd1.Init.ClockEdge           = SDMMC_CLOCK_EDGE_RISING;
  hsd1.Init.ClockBypass         = SDMMC_CLOCK_BYPASS_DISABLE;
  hsd1.Init.ClockPowerSave      = SDMMC_CLOCK_POWER_SAVE_DISABLE;
  hsd1.Init.BusWide             = SDMMC_BUS_WIDE_1B;   /* nhận dạng thẻ luôn ở 1-bit */
  hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
  hsd1.Init.ClockDiv            = SD_RUN_CLKDIV;

  if (HAL_SD_Init(&hsd1) != HAL_OK)
  {
    g_sd_init_step = 1U;
  }
  else
  {
    sd_dma_init();
    sd_set_clock(SD_RUN_CLKDIV, 0U);
    if ((HAL_SD_ConfigWideBusOperation(&hsd1, SDMMC_BUS_WIDE_4B) != HAL_OK) ||
        (sd_wait_transfer(SD_READY_TIMEOUT_MS) == 0U))
    {
      g_sd_init_step = 2U;
    }
    else if (sd_verify() == 0U)
    {
      g_sd_init_step = 3U;
    }
  }

  if (g_sd_init_step != 0U)
  {
    static const char *const step[4] = { "", "CARD INIT", "4-BIT SWITCH", "4-BIT 25MHz TEST READ" };
    uint32_t err = (g_sd_init_step == 3U) ? g_sd_verify_err : hsd1.ErrorCode;

    g_sd_err_4bit = err;
    App_LCD_Log(LCD_COLOR_RED, "SD %s FAULT err=0x%lX %s", step[g_sd_init_step],
                (unsigned long)err, sd_err_str(err));
    return MSD_ERROR;
  }

  g_sd_bus_width = 4U;
  return MSD_OK;
}

/* Đọc sector cho FatFs (bằng DMA, 4-bit 25 MHz cố định). Lỗi không bị bỏ qua:
 * in lên màn hình (loại lỗi, block), khôi phục thẻ và đọc lại tối đa 2 lần ở cùng tốc độ.
 * Timeout của sd_diskio rất lớn nên giới hạn SD_IO_TIMEOUT_MS cho mỗi lần DMA. */
#define SD_RT_LOG_MAX   8U        /* chỉ in tối đa chừng này lỗi, tránh phủ kín ảnh */

uint8_t BSP_SD_ReadBlocks(uint32_t *pData, uint32_t ReadAddr, uint32_t NumOfBlocks, uint32_t Timeout)
{
  uint32_t tries;
  uint32_t tmo = (Timeout < SD_IO_TIMEOUT_MS) ? Timeout : SD_IO_TIMEOUT_MS;

  for (tries = 0U; tries < 3U; tries++)
  {
    uint32_t err;

    if (sd_read_any((uint8_t *)pData, ReadAddr, NumOfBlocks, tmo) != 0U)
    {
      return MSD_OK;
    }
    err = hsd1.ErrorCode;
    g_sd_read_retry++;
    g_sd_rt_err++;
    g_sd_last_read_err = err;
    if (g_sd_rt_err <= SD_RT_LOG_MAX)
    {
      App_LCD_Log(LCD_COLOR_RED, "SD READ ERR blk %lu n %lu err=0x%lX %s (try %lu)",
                  (unsigned long)ReadAddr, (unsigned long)NumOfBlocks,
                  (unsigned long)err, sd_err_str(err), (unsigned long)(tries + 1U));
    }
    sd_recover();
  }
  return MSD_ERROR;
}


/* ---------------------------------------------------------------------------
 * Đo tốc độ đọc thẻ: đọc APP_SDCARD_BENCH_KB từ đầu thẻ vào buffer JPEG ở SDRAM
 * qua đúng đường BSP_SD_ReadBlocks (DMA) mà FatFs dùng.
 * ------------------------------------------------------------------------- */
volatile uint32_t g_sd_bench_kBps;        /* tốc độ đọc thô đo lúc mount (KB/s, 1 KB = 1000 B) */

static void sd_bench(void)
{
#if (APP_SDCARD_BENCH_KB != 0U)
  const uint32_t chunk_blk = APP_SDRAM_JPEG_BUF_SIZE / 512U;
  uint32_t total_blk = (APP_SDCARD_BENCH_KB * 1024U) / 512U;
  uint32_t blk = 0U, t0, ms;

  g_sd_bench_kBps = 0U;
  if (hsd1.SdCard.LogBlockNbr < total_blk)
  {
    return;
  }
  t0 = HAL_GetTick();
  while (blk < total_blk)
  {
    uint32_t n = ((total_blk - blk) > chunk_blk) ? chunk_blk : (total_blk - blk);

    if ((BSP_SD_ReadBlocks((uint32_t *)APP_SDRAM_JPEG_BUF_ADDR, blk, n, SD_IO_TIMEOUT_MS) != MSD_OK) ||
        (sd_wait_transfer(SD_READY_TIMEOUT_MS) == 0U))
    {
      App_LCD_Log(LCD_COLOR_RED, "SD SPEED TEST FAULT at block %lu (err=0x%08lX)",
                  (unsigned long)blk, (unsigned long)g_sd_last_read_err);
      return;
    }
    blk += n;
  }
  ms = HAL_GetTick() - t0;
  if (ms == 0U)
  {
    ms = 1U;
  }
  g_sd_bench_kBps = (total_blk * 512U) / ms;
  App_LCD_Log(LCD_COLOR_GREEN, "SD READ SPEED %lu.%02lu MB/s (%lu KB in %lu ms, DMA%s)",
              (unsigned long)(g_sd_bench_kBps / 1000U), (unsigned long)((g_sd_bench_kBps % 1000U) / 10U),
              (unsigned long)APP_SDCARD_BENCH_KB, (unsigned long)ms,
              (sd_dma_ready != 0U) ? "" : " OFF");
#endif
}

/* ---------------------------------------------------------------------------
 * Nguồn dữ liệu cho LibJPEG: đọc file theo khối lớn vào buffer ở SDRAM.
 * jpeg_stdio_src mặc định chỉ đọc 4 KB mỗi lần (8 sector) -> nhiều lệnh SD nhỏ.
 * Ở đây mỗi lần f_read lấy APP_SDRAM_JPEG_BUF_SIZE; vị trí đọc luôn chẵn sector
 * nên FatFs đọc thẳng nhiều sector liên tiếp vào buffer (DMA, mỗi lệnh tới hết
 * một cluster).
 * ------------------------------------------------------------------------- */
typedef struct
{
  struct jpeg_source_mgr pub;   /* phải là phần tử đầu tiên */
  FIL                   *fp;
} sd_jsrc_t;

static sd_jsrc_t     sd_jsrc;
static const JOCTET  sd_jsrc_eoi[2] = { 0xFFU, (JOCTET)JPEG_EOI };
volatile uint32_t    g_sd_io_bytes;       /* số byte đọc từ thẻ cho ảnh gần nhất */
volatile uint32_t    g_sd_io_ms;          /* thời gian đọc thẻ cho ảnh gần nhất (ms) */

static void sd_jsrc_init(j_decompress_ptr cinfo)
{
  (void)cinfo;
}

static boolean sd_jsrc_fill(j_decompress_ptr cinfo)
{
  sd_jsrc_t *src = (sd_jsrc_t *)cinfo->src;
  UINT       br  = 0U;
  uint32_t   t0  = HAL_GetTick();
  FRESULT    fr;

  fr = f_read(src->fp, (void *)APP_SDRAM_JPEG_BUF_ADDR, APP_SDRAM_JPEG_BUF_SIZE, &br);
  g_sd_io_ms    += HAL_GetTick() - t0;
  g_sd_io_bytes += br;

  if ((fr != FR_OK) || (br == 0U))
  {
    g_sd_last_fresult = fr;
    WARNMS(cinfo, JWRN_JPEG_EOF);              /* chèn EOI giả, LibJPEG tự kết thúc */
    src->pub.next_input_byte = sd_jsrc_eoi;
    src->pub.bytes_in_buffer = sizeof(sd_jsrc_eoi);
    return TRUE;
  }
  src->pub.next_input_byte = (const JOCTET *)APP_SDRAM_JPEG_BUF_ADDR;
  src->pub.bytes_in_buffer = br;
  return TRUE;
}

static void sd_jsrc_skip(j_decompress_ptr cinfo, long num_bytes)
{
  sd_jsrc_t *src = (sd_jsrc_t *)cinfo->src;

  if (num_bytes <= 0)
  {
    return;
  }
  while (num_bytes > (long)src->pub.bytes_in_buffer)
  {
    num_bytes -= (long)src->pub.bytes_in_buffer;
    (void)sd_jsrc_fill(cinfo);
  }
  src->pub.next_input_byte += (size_t)num_bytes;
  src->pub.bytes_in_buffer -= (size_t)num_bytes;
}

static void sd_jsrc_term(j_decompress_ptr cinfo)
{
  (void)cinfo;
}

/* Gắn nguồn dữ liệu (gọi sau jpeg_create_decompress) */
static void sd_jsrc_attach(j_decompress_ptr cinfo, FIL *fp)
{
  sd_jsrc.pub.init_source       = sd_jsrc_init;
  sd_jsrc.pub.fill_input_buffer = sd_jsrc_fill;
  sd_jsrc.pub.skip_input_data   = sd_jsrc_skip;
  sd_jsrc.pub.resync_to_restart = jpeg_resync_to_restart;
  sd_jsrc.pub.term_source       = sd_jsrc_term;
  sd_jsrc.pub.next_input_byte   = NULL;
  sd_jsrc.pub.bytes_in_buffer   = 0U;
  sd_jsrc.fp                    = fp;
  g_sd_io_bytes = 0U;
  g_sd_io_ms    = 0U;
  cinfo->src = &sd_jsrc.pub;
}

/* LibJPEG gọi hàm này khi gặp lỗi: lưu thông báo rồi nhảy về điểm setjmp */
static void sd_jpeg_error_exit(j_common_ptr cinfo)
{
  sd_jpeg_err_t *err = (sd_jpeg_err_t *)cinfo->err;

  (*cinfo->err->format_message)(cinfo, sd_jpeg_msg);
  longjmp(err->jump, 1);
}

/* Bỏ qua cảnh báo của LibJPEG (mặc định sẽ in ra stderr) */
static void sd_jpeg_output_message(j_common_ptr cinfo)
{
  (void)cinfo;
}

static const char *sd_fresult_str(FRESULT fr)
{
  static const char *const names[] =
  {
    "OK", "DISK_ERR", "INT_ERR", "NOT_READY", "NO_FILE", "NO_PATH",
    "INVALID_NAME", "DENIED", "EXIST", "INVALID_OBJECT", "WRITE_PROTECTED",
    "INVALID_DRIVE", "NOT_ENABLED", "NO_FILESYSTEM", "MKFS_ABORTED", "TIMEOUT",
    "LOCKED", "NOT_ENOUGH_CORE", "TOO_MANY_OPEN_FILES", "INVALID_PARAMETER"
  };

  return ((uint32_t)fr < (sizeof(names) / sizeof(names[0]))) ? names[fr] : "?";
}

/* Phần tên file (bỏ "0:/") để in cho gọn */
static const char *sd_basename(const char *path)
{
  const char *p = strrchr(path, '/');

  return (p != NULL) ? (p + 1) : path;
}

static char sd_tolower(char c)
{
  return ((c >= 'A') && (c <= 'Z')) ? (char)(c - 'A' + 'a') : c;
}

/* 1 nếu tên file kết thúc bằng .jpg hoặc .jpeg (không phân biệt hoa thường) */
static uint8_t sd_is_jpeg(const char *name)
{
  static const char *const ext[] = { ".jpg", ".jpeg" };
  size_t n = strlen(name);
  uint32_t e;

  for (e = 0U; e < (sizeof(ext) / sizeof(ext[0])); e++)
  {
    size_t m = strlen(ext[e]);
    size_t i;

    if (n <= m)
    {
      continue;
    }
    for (i = 0U; i < m; i++)
    {
      if (sd_tolower(name[n - m + i]) != ext[e][i])
      {
        break;
      }
    }
    if (i == m)
    {
      return 1U;
    }
  }
  return 0U;
}

/* Liệt kê ảnh JPEG ở thư mục gốc. Trả về kết quả f_opendir. */
static FRESULT sd_scan(uint32_t *total_files)
{
  static DIR     dir;
  static FILINFO fno;
  FRESULT fr;

  sd_count = 0U;
  *total_files = 0U;
  fr = f_opendir(&dir, SDPath);
  if (fr != FR_OK)
  {
    return fr;
  }

  for (;;)
  {
    fr = f_readdir(&dir, &fno);
    if ((fr != FR_OK) || (fno.fname[0] == '\0'))
    {
      break;
    }
    (*total_files)++;
    if ((fno.fattrib & (AM_DIR | AM_HID | AM_SYS)) != 0U)
    {
      continue;
    }
    if ((fno.fname[0] == '.') || (sd_is_jpeg(fno.fname) == 0U))
    {
      continue;   /* bỏ qua file ẩn kiểu "._abc.jpg" của macOS và file không phải JPEG */
    }
    if ((strlen(SDPath) + strlen(fno.fname)) >= APP_SDCARD_PATH_LEN)
    {
      continue;   /* tên quá dài */
    }
    (void)snprintf(sd_files[sd_count], APP_SDCARD_PATH_LEN, "%s%s", SDPath, fno.fname);
    sd_count++;
    if (sd_count >= APP_SDCARD_MAX_FILES)
    {
      break;
    }
  }
  (void)f_closedir(&dir);
  return FR_OK;
}

/* Chỉ in log khi trạng thái thay đổi, tránh in lặp mỗi 3 s khi đang chờ thẻ */
static uint8_t sd_status_changed(sd_app_status_t st)
{
  g_sd_status = st;
  if (st == sd_logged_status)
  {
    return 0U;
  }
  sd_logged_status = st;
  return 1U;
}

/* Mount thẻ và liệt kê ảnh. Trả về 1 nếu có ít nhất một ảnh. */
static uint8_t sd_mount_and_scan(void)
{
  FRESULT fr;
  uint32_t total;

  if (sd_mounted == 0U)
  {
    /* Bước 1: chân phát hiện thẻ */
    if (BSP_SD_IsDetected() != SD_PRESENT)
    {
      if (sd_status_changed(SD_APP_NO_CARD) != 0U)
      {
        App_LCD_Log(LCD_COLOR_RED, "SDCARD NOT DETECTED (PD4 = %u)",
                    (unsigned)LL_GPIO_IsInputPinSet(SD_DET_GPIO_Port, SD_DET_Pin));
        App_LCD_Log(LCD_COLOR_YELLOW, "  -> insert card, retry every %u s",
                    (unsigned)(APP_SDCARD_RETRY_MS / 1000U));
      }
      return 0U;
    }

    /* Bước 2: khởi tạo thẻ + mount FAT */
    App_LCD_Log(LCD_COLOR_WHITE, "SDCARD DETECTED, INIT + MOUNT ...");
    fr = f_mount(&SDFatFS, SDPath, 1);
    g_sd_last_fresult = fr;
    if (fr != FR_OK)
    {
      sd_logged_status = SD_APP_NO_CARD;
      g_sd_status = SD_APP_NO_CARD;
      if (fr == FR_NO_FILESYSTEM)
      {
        App_LCD_Log(LCD_COLOR_RED, "SDCARD MOUNT FAULT: NO FAT32 (exFAT/NTFS?)");
        App_LCD_Log(LCD_COLOR_YELLOW, "  -> format card as FAT32");
      }
      else
      {
        App_LCD_Log(LCD_COLOR_RED, "SDCARD INIT FAULT: FR=%u %s",
                    (unsigned)fr, sd_fresult_str(fr));
        App_LCD_Log(LCD_COLOR_RED, "  HAL SD state=%u err=0x%08lX (4-bit err=0x%08lX)",
                    (unsigned)hsd1.State, (unsigned long)hsd1.ErrorCode,
                    (unsigned long)g_sd_err_4bit);
        App_LCD_Log(LCD_COLOR_YELLOW, "  err 0x2=DATA CRC, 0x8=DATA TIMEOUT, 0x20=RX OVERRUN");
      }
      (void)f_mount(NULL, SDPath, 0);
      return 0U;
    }
    sd_mounted = 1U;
    App_LCD_Log(LCD_COLOR_GREEN, "SDCARD MOUNT OK (%lu MB, %lu-bit, %lu kHz%s%s)",
                (unsigned long)(((uint64_t)hsd1.SdCard.BlockNbr * hsd1.SdCard.BlockSize) >> 20),
                (unsigned long)g_sd_bus_width, (unsigned long)g_sd_clk_khz,
                (g_sd_high_speed != 0U) ? " HS" : "",
                (hsd1.Init.ClockEdge == SDMMC_CLOCK_EDGE_FALLING) ? " NEGEDGE" : "");
    sd_bench();
  }

  /* Bước 3: liệt kê ảnh */
  fr = sd_scan(&total);
  g_sd_last_fresult = fr;
  g_sd_jpg_count = sd_count;
  sd_next = 0U;
  if (fr != FR_OK)
  {
    App_LCD_Log(LCD_COLOR_RED, "SDCARD READ DIR FAULT: FR=%u %s",
                (unsigned)fr, sd_fresult_str(fr));
    (void)f_mount(NULL, SDPath, 0);
    sd_mounted = 0U;
    g_sd_status = SD_APP_NO_CARD;
    return 0U;
  }

  if (sd_count == 0U)
  {
    if (sd_status_changed(SD_APP_NO_IMAGE) != 0U)
    {
      App_LCD_Log(LCD_COLOR_RED, "NO JPG FILE IN ROOT (%lu entries)", (unsigned long)total);
      App_LCD_Log(LCD_COLOR_YELLOW, "  -> copy .jpg/.jpeg to root of card");
    }
    return 0U;
  }

  (void)sd_status_changed(SD_APP_OK);
  App_LCD_Log(LCD_COLOR_GREEN, "FOUND %lu JPG FILE(S) - touch screen for next image",
              (unsigned long)sd_count);
  return 1U;
}

/* Bỏ mount khi thẻ bị rút / lỗi đọc, để lần sau mount lại */
static void sd_unmount(void)
{
  (void)f_mount(NULL, SDPath, 0);
  sd_mounted = 0U;
  sd_count = 0U;
  g_sd_jpg_count = 0U;
}

static inline uint16_t sd_rgb565(const JSAMPLE *p)
{
  return (uint16_t)((((uint16_t)p[0] & 0xF8U) << 8) |
                    (((uint16_t)p[1] & 0xFCU) << 3) |
                    ((uint16_t)p[2] >> 3));
}

/* Xoá vùng viền ngoài khung ảnh bằng DMA2D (4 hình chữ nhật, không xoá cả 1.2 MB).
 * Gọi TRƯỚC khi CPU vẽ ảnh vào fb (DMA2D chạy xong mới trả về). */
static void sd_clear_border(uint16_t *fb, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h)
{
  App_LCD_FillRect(fb, 0U, 0U, APP_LCD_WIDTH, y0, LCD_COLOR_BLACK);                       /* trên */
  App_LCD_FillRect(fb, 0U, y0 + h, APP_LCD_WIDTH, APP_LCD_HEIGHT - (y0 + h), LCD_COLOR_BLACK); /* dưới */
  App_LCD_FillRect(fb, 0U, y0, x0, h, LCD_COLOR_BLACK);                                   /* trái */
  App_LCD_FillRect(fb, x0 + w, y0, APP_LCD_WIDTH - (x0 + w), h, LCD_COLOR_BLACK);         /* phải */
}

/* Giải mã bằng LibJPEG (phần mềm) vào framebuffer fb (chưa hiển thị). Dùng cho ảnh
 * codec phần cứng không giải mã được (progressive...), và cho bước tự kiểm tra.
 * full_res = 1: không cho LibJPEG thu nhỏ, IDCT chính xác (để so với phần cứng).
 * Trả về 0: thành công, 1: không mở được file, 2: lỗi giải mã. */
static uint32_t sd_decode_jpeg(const char *path, uint16_t *fb, uint8_t full_res)
{
  static uint16_t xmap[APP_LCD_WIDTH];   /* cột đích -> vị trí byte trong dòng nguồn */
  JSAMPARRAY row;
  uint32_t src_w, src_h, fit_w, fit_h, out_w, out_h, num;
  uint32_t x0, y0, sy, dy, dx;

  g_sd_last_fresult = f_open(&sd_jpg_file, path, FA_READ);
  if (g_sd_last_fresult != FR_OK)
  {
    return 1U;
  }

  sd_jpeg_msg[0] = '\0';
  sd_img_w = 0U;
  sd_img_h = 0U;
  sd_img_progressive = 0U;
  sd_cinfo.err = jpeg_std_error(&sd_jerr.pub);
  sd_jerr.pub.error_exit     = sd_jpeg_error_exit;
  sd_jerr.pub.output_message = sd_jpeg_output_message;
  if (setjmp(sd_jerr.jump) != 0)
  {
    /* Lỗi giải mã (file hỏng, JPEG progressive quá lớn, hết heap...) */
    jpeg_destroy_decompress(&sd_cinfo);
    (void)f_close(&sd_jpg_file);
    return 2U;
  }

  jpeg_create_decompress(&sd_cinfo);
  sd_jsrc_attach(&sd_cinfo, &sd_jpg_file);   /* đọc khối lớn vào SDRAM thay cho jpeg_stdio_src */
  (void)jpeg_read_header(&sd_cinfo, TRUE);

  /* Kích thước hiển thị: vừa màn hình, giữ tỉ lệ */
  src_w = sd_cinfo.image_width;
  src_h = sd_cinfo.image_height;
  sd_img_w = src_w;
  sd_img_h = src_h;
  sd_img_progressive = (sd_cinfo.progressive_mode != FALSE) ? 1U : 0U;
  if ((src_w * APP_LCD_HEIGHT) >= (src_h * APP_LCD_WIDTH))
  {
    fit_w = APP_LCD_WIDTH;
    fit_h = (src_h * APP_LCD_WIDTH) / src_w;
  }
  else
  {
    fit_h = APP_LCD_HEIGHT;
    fit_w = (src_w * APP_LCD_HEIGHT) / src_h;
  }
  if (fit_w == 0U) { fit_w = 1U; }
  if (fit_h == 0U) { fit_h = 1U; }

  /* Cho LibJPEG thu nhỏ sẵn (scale_num/8) tới mức nhỏ nhất vẫn >= kích thước hiển thị,
     giảm đáng kể thời gian giải mã ảnh lớn */
  num = (full_res != 0U) ? 8U : 1U;
  while ((num < 8U) &&
         ((((src_w * num) + 7U) / 8U < fit_w) || (((src_h * num) + 7U) / 8U < fit_h)))
  {
    num++;
  }
  sd_cinfo.scale_num           = num;
  sd_cinfo.scale_denom         = 8U;
  sd_cinfo.out_color_space     = JCS_RGB;
  sd_cinfo.dct_method          = (full_res != 0U) ? JDCT_ISLOW : JDCT_IFAST;
  sd_cinfo.do_fancy_upsampling = FALSE;

  (void)jpeg_start_decompress(&sd_cinfo);
  out_w = sd_cinfo.output_width;
  out_h = sd_cinfo.output_height;

  row = (*sd_cinfo.mem->alloc_sarray)((j_common_ptr)&sd_cinfo, JPOOL_IMAGE,
                                      out_w * (uint32_t)sd_cinfo.output_components, 1);

  /* Bảng tra cột: tránh phép chia cho từng pixel */
  for (dx = 0U; dx < fit_w; dx++)
  {
    xmap[dx] = (uint16_t)(((dx * out_w) / fit_w) * 3U);
  }

  x0 = (APP_LCD_WIDTH  - fit_w) / 2U;
  y0 = (APP_LCD_HEIGHT - fit_h) / 2U;
  sd_clear_border(fb, x0, y0, fit_w, fit_h);

  /* Đọc từng dòng nguồn; mỗi dòng đích dy lấy dòng nguồn dy*out_h/fit_h (nearest) */
  dy = 0U;
  for (sy = 0U; (sy < out_h) && (dy < fit_h); sy++)
  {
    (void)jpeg_read_scanlines(&sd_cinfo, row, 1);

    while ((dy < fit_h) && (((dy * out_h) / fit_h) == sy))
    {
      uint16_t *dst = fb + ((y0 + dy) * APP_LCD_WIDTH) + x0;
      const JSAMPLE *src = row[0];

      for (dx = 0U; dx < fit_w; dx++)
      {
        dst[dx] = sd_rgb565(&src[xmap[dx]]);
      }
      dy++;
    }
  }

  /* Không cần đọc hết phần còn lại của ảnh */
  jpeg_abort_decompress(&sd_cinfo);
  jpeg_destroy_decompress(&sd_cinfo);
  (void)f_close(&sd_jpg_file);
  return 0U;
}

/* ===========================================================================
 * Giải mã ảnh: codec JPEG phần cứng trước, LibJPEG khi phần cứng không hỗ trợ
 * ======================================================================== */
#define SD_PATH_HW   1U
#define SD_PATH_SW   2U

/* Thống kê ảnh vừa giải mã (xem bằng debugger) */
volatile uint32_t g_img_path;        /* 1 = codec phần cứng, 2 = LibJPEG */
volatile uint32_t g_img_w, g_img_h;
volatile uint32_t g_img_kb;          /* kích thước file (KB) */
volatile uint32_t g_img_read_ms;     /* thời gian đọc thẻ */
volatile uint32_t g_img_dec_ms;      /* thời gian giải mã + chuyển màu + thu nhỏ */
volatile uint32_t g_img_total_ms;
volatile uint32_t g_jpeg_hw_count;   /* số ảnh giải mã bằng phần cứng */
volatile uint32_t g_jpeg_sw_count;   /* số ảnh phải dùng LibJPEG */
volatile uint32_t g_jpeg_hw_fail;    /* số lần phần cứng lỗi/treo rồi chuyển sang LibJPEG */
volatile uint32_t g_jpeg_selftest;   /* 0 = chưa chạy, 1 = PASS, 2 = FAIL, 3 = bỏ qua */

static app_jpeg_info_t sd_hw_info;
static uint32_t        sd_sof;                         /* marker SOF của ảnh vừa đọc */
static char            sd_stat1[96];
static char            sd_stat2[96];

/* Hàm đọc cho codec phần cứng: đọc tiếp file vào buffer SDRAM (DMA) */
static uint32_t sd_hw_read(void *ctx, uint8_t *buf, uint32_t size)
{
  UINT br = 0U;
  FRESULT fr = f_read((FIL *)ctx, buf, size, &br);

  if (fr != FR_OK)
  {
    g_sd_last_fresult = fr;
    return 0xFFFFFFFFU;
  }
  return (uint32_t)br;
}

static const char *sd_subsamp_str(const app_jpeg_info_t *i)
{
  if (i->ncomp == 1U)                   { return "gray"; }
  if ((i->hsf == 2U) && (i->vsf == 2U)) { return "4:2:0"; }
  if ((i->hsf == 2U) && (i->vsf == 1U)) { return "4:2:2"; }
  if ((i->hsf == 1U) && (i->vsf == 2U)) { return "4:4:0"; }
  return "4:4:4";
}

/* Giải mã path vào fb. Trả về 0: OK, 1: lỗi đọc file, 2: lỗi giải mã. */
static uint32_t sd_decode_image(const char *path, uint16_t *fb)
{
  uint8_t *buf = (uint8_t *)APP_SDRAM_JPEG_BUF_ADDR;
  uint32_t t0 = HAL_GetTick();
  uint32_t w = 0U, h = 0U, r, t_read;
  int32_t  pr;
  UINT     br = 0U;
  app_jpeg_fit_t fit;

  sd_stat1[0] = '\0';
  sd_sof = 0U;
  g_sd_last_fresult = f_open(&sd_jpg_file, path, FA_READ);
  if (g_sd_last_fresult != FR_OK)
  {
    return 1U;
  }
  g_img_kb = (uint32_t)(f_size(&sd_jpg_file) / 1024U);

  /* Khối đầu (tới 1 MB) đọc thẳng vào SDRAM bằng DMA */
  g_sd_last_fresult = f_read(&sd_jpg_file, buf, APP_SDRAM_JPEG_BUF_SIZE, &br);
  t_read = HAL_GetTick() - t0;
  if (g_sd_last_fresult != FR_OK)
  {
    (void)f_close(&sd_jpg_file);
    return 1U;
  }

  pr = App_JPEG_Probe(buf, (uint32_t)br, &w, &h, &sd_sof);
  if (pr == 1)
  {
    App_JPEG_Fit(w, h, &fit);
    sd_clear_border(fb, fit.x0, fit.y0, fit.w, fit.h);
    r = App_JPEG_DecodeHW(buf, (uint32_t)br, APP_SDRAM_JPEG_BUF_SIZE, sd_hw_read, &sd_jpg_file,
                          fb, &fit, &sd_hw_info);
    (void)f_close(&sd_jpg_file);
    if (r == APP_JPEG_OK)
    {
      g_img_path     = SD_PATH_HW;
      g_img_w        = sd_hw_info.width;
      g_img_h        = sd_hw_info.height;
      g_img_read_ms  = t_read + sd_hw_info.read_ms;
      g_img_dec_ms   = sd_hw_info.ms;
      g_img_total_ms = HAL_GetTick() - t0;
      g_jpeg_hw_count++;
      (void)snprintf(sd_stat1, sizeof(sd_stat1),
                     "HW JPEG  %lux%lu %s  %lu KB  read %lu ms  decode %lu ms  total %lu ms",
                     (unsigned long)g_img_w, (unsigned long)g_img_h, sd_subsamp_str(&sd_hw_info),
                     (unsigned long)g_img_kb, (unsigned long)g_img_read_ms,
                     (unsigned long)g_img_dec_ms, (unsigned long)g_img_total_ms);
      return 0U;
    }
    if (r == APP_JPEG_READ_ERR)
    {
      return 1U;
    }
    if (r == APP_JPEG_TIMEOUT)
    {
      g_jpeg_hw_fail++;
      App_LCD_Log(LCD_COLOR_YELLOW, "HW JPEG STALL (SR=0x%lX) %s -> LibJPEG",
                  (unsigned long)g_jpeg_hw_sr, sd_basename(path));
    }
  }
  else
  {
    (void)f_close(&sd_jpg_file);
  }

  /* LibJPEG: progressive / CMYK / lấy mẫu lạ / phần cứng lỗi */
  t0 = HAL_GetTick();
  r = sd_decode_jpeg(path, fb, 0U);
  if (r == 0U)
  {
    g_img_path     = SD_PATH_SW;
    g_img_w        = sd_img_w;
    g_img_h        = sd_img_h;
    g_img_read_ms  = g_sd_io_ms;
    g_img_dec_ms   = (HAL_GetTick() - t0) - g_sd_io_ms;
    g_img_total_ms = HAL_GetTick() - t0;
    g_jpeg_sw_count++;
    (void)snprintf(sd_stat1, sizeof(sd_stat1),
                   "LibJPEG (%s)  %lux%lu  %lu KB  read %lu ms  decode %lu ms  total %lu ms",
                   (sd_img_progressive != 0U) ? "progressive" : ((sd_sof != 0U) ? "unsupported" : "?"),
                   (unsigned long)g_img_w, (unsigned long)g_img_h, (unsigned long)g_img_kb,
                   (unsigned long)g_img_read_ms, (unsigned long)g_img_dec_ms,
                   (unsigned long)g_img_total_ms);
  }
  return r;
}

#if (APP_SDCARD_JPEG_SELFTEST != 0U)
/* So ảnh phần cứng (fb) với LibJPEG độ phân giải đầy đủ (tmp) trong khung hiển thị.
 * Cùng cách lấy mẫu nearest và upsampling chroma kiểu lặp, nên chỉ khác do làm tròn
 * IDCT/chuyển màu: trung bình phải ~0, tối đa vài đơn vị RGB565. */
static void sd_jpeg_selftest(const char *path, const uint16_t *fb)
{
  uint16_t *tmp;
  app_jpeg_fit_t fit;
  uint32_t x, y, t0, sw_ms, n = 0U;
  uint32_t sum[3] = { 0U, 0U, 0U }, mx[3] = { 0U, 0U, 0U };

  g_jpeg_selftest = 3U;
  tmp = (uint16_t *)malloc(APP_LCD_WIDTH * APP_LCD_HEIGHT * 2U);
  if (tmp == NULL)
  {
    (void)snprintf(sd_stat2, sizeof(sd_stat2), "SELFTEST skipped: no heap");
    return;
  }
  t0 = HAL_GetTick();
  if (sd_decode_jpeg(path, tmp, 1U) != 0U)
  {
    free(tmp);
    (void)snprintf(sd_stat2, sizeof(sd_stat2), "SELFTEST skipped: LibJPEG error %s", sd_jpeg_msg);
    return;
  }
  sw_ms = HAL_GetTick() - t0;
  SCB_CleanInvalidateDCache();                         /* tmp được DMA2D tô viền */

  App_JPEG_Fit(g_img_w, g_img_h, &fit);
  for (y = fit.y0; y < (fit.y0 + fit.h); y++)
  {
    for (x = fit.x0; x < (fit.x0 + fit.w); x++)
    {
      uint32_t a = fb[(y * APP_LCD_WIDTH) + x], b = tmp[(y * APP_LCD_WIDTH) + x];
      int32_t  d[3];
      uint32_t k;

      d[0] = (int32_t)((a >> 11) & 31U) - (int32_t)((b >> 11) & 31U);
      d[1] = (int32_t)((a >> 5) & 63U)  - (int32_t)((b >> 5) & 63U);
      d[2] = (int32_t)(a & 31U)         - (int32_t)(b & 31U);
      for (k = 0U; k < 3U; k++)
      {
        uint32_t v = (uint32_t)((d[k] < 0) ? -d[k] : d[k]);
        sum[k] += v;
        if (v > mx[k]) { mx[k] = v; }
      }
      n++;
    }
  }
  free(tmp);
  if (n == 0U) { n = 1U; }

  /* PASS: sai khác trung bình mỗi kênh <= 0.25 đơn vị RGB565 */
  g_jpeg_selftest = (((sum[0] * 4U) <= n) && ((sum[1] * 4U) <= n) && ((sum[2] * 4U) <= n)) ? 1U : 2U;
  (void)snprintf(sd_stat2, sizeof(sd_stat2),
                 "SELFTEST HW vs LibJPEG: %s  avg R/G/B %lu.%02lu/%lu.%02lu/%lu.%02lu  max %lu/%lu/%lu  (SW %lu ms)",
                 (g_jpeg_selftest == 1U) ? "PASS" : "FAIL",
                 (unsigned long)(sum[0] / n), (unsigned long)(((sum[0] % n) * 100U) / n),
                 (unsigned long)(sum[1] / n), (unsigned long)(((sum[1] % n) * 100U) / n),
                 (unsigned long)(sum[2] / n), (unsigned long)(((sum[2] % n) * 100U) / n),
                 (unsigned long)mx[0], (unsigned long)mx[1], (unsigned long)mx[2],
                 (unsigned long)sw_ms);
}
#endif

/* Vẽ dòng thống kê ở góc dưới trái ảnh (font 8x16, nền đen) */
static void sd_draw_stats(uint16_t *fb)
{
#if (APP_SDCARD_SHOW_STATS != 0U)
  uint32_t y = APP_LCD_HEIGHT - 20U;

  if (sd_stat2[0] != '\0')
  {
    App_LCD_DrawText(fb, 4U, y - 18U, sd_stat1, LCD_COLOR_GREEN, LCD_COLOR_BLACK, 1U);
    App_LCD_DrawText(fb, 4U, y, sd_stat2,
                     (g_jpeg_selftest == 2U) ? LCD_COLOR_RED : LCD_COLOR_YELLOW, LCD_COLOR_BLACK, 1U);
  }
  else
  {
    App_LCD_DrawText(fb, 4U, y, sd_stat1, LCD_COLOR_GREEN, LCD_COLOR_BLACK, 1U);
  }
#else
  (void)fb;
#endif
}

/* Giải mã ảnh kế tiếp (từ sd_next, bỏ qua ảnh lỗi) vào back buffer.
 * Trả về 1 nếu back buffer đã có ảnh, 0 nếu không có ảnh nào giải mã được. */
static uint8_t sd_decode_next_to_back(void)
{
  uint32_t tries;

  for (tries = 0U; tries < sd_count; tries++)
  {
    uint32_t idx = sd_next;
    const char *name = sd_basename(sd_files[idx]);
    uint32_t t0 = HAL_GetTick();
    uint32_t err;

    sd_next = (sd_next + 1U) % sd_count;
    if (sd_shown_once == 0U)
    {
      App_LCD_Log(LCD_COLOR_WHITE, "DECODE %lu/%lu: %s",
                  (unsigned long)(idx + 1U), (unsigned long)sd_count, name);
    }

    err = sd_decode_image(sd_files[idx], App_LCD_GetBackBuffer());
    if (err == 0U)
    {
      sd_back_idx = idx;
      g_sd_last_ms = HAL_GetTick() - t0;
      sd_stat2[0] = '\0';
#if (APP_SDCARD_JPEG_SELFTEST != 0U)
      if ((g_jpeg_selftest == 0U) && (g_img_path == SD_PATH_HW))
      {
        sd_jpeg_selftest(sd_files[idx], App_LCD_GetBackBuffer());
      }
#endif
      sd_draw_stats(App_LCD_GetBackBuffer());
      return 1U;
    }

    g_sd_fail_count++;
    if (err == 1U)
    {
      /* Không mở được file: có thể thẻ đã bị rút */
      App_LCD_Log(LCD_COLOR_RED, "IMAGE READ FAULT: %s", name);
      App_LCD_Log(LCD_COLOR_RED, "  FR=%u %s", (unsigned)g_sd_last_fresult,
                  sd_fresult_str(g_sd_last_fresult));
      sd_unmount();
      sd_logged_status = (sd_app_status_t)0xFF;
      g_sd_status = SD_APP_NO_CARD;
      return 0U;
    }

    App_LCD_Log(LCD_COLOR_RED, "IMAGE DECODE FAULT: %s", name);
    App_LCD_Log(LCD_COLOR_RED, "  %s", sd_jpeg_msg);
    if (sd_img_w != 0U)
    {
      App_LCD_Log(LCD_COLOR_YELLOW, "  image %lux%lu, %s", (unsigned long)sd_img_w,
                  (unsigned long)sd_img_h, (sd_img_progressive != 0U) ? "PROGRESSIVE" : "baseline");
    }
  }
  return 0U;
}

/* Hiện ảnh đang có trong back buffer, sau đó cần tải sẵn ảnh kế tiếp */
static void sd_present_back(void)
{
  App_LCD_SwapBuffer();
  g_sd_jpg_index = sd_back_idx;
  sd_shown_once = 1U;
  sd_back_ready = 0U;
  sd_need_preload = (sd_count > 1U) ? 1U : 0U;
}


void App_SDCARD_Init(void)
{
  sd_mounted = 0U;
  sd_count = 0U;
  sd_next = 0U;
  sd_shown_once = 0U;
  sd_back_ready = 0U;
  sd_need_preload = 0U;
  sd_retry_ms = HAL_GetTick();

  if (App_LCD_IsReady() == 0U)
  {
    g_sd_status = SD_APP_NO_LCD;   /* màn hình đang báo đỏ (lỗi SDRAM) */
    return;
  }
  if ((sd_mount_and_scan() != 0U) && (sd_decode_next_to_back() != 0U))
  {
    sd_present_back();   /* hiện ảnh đầu tiên ngay khi khởi động */
  }
}

void App_SDCARD_Task(void *pvParam)
{
  uint8_t tap;

  (void)pvParam;

  tap = App_TOUCH_GetTap();   /* lấy sự kiện chạm (xoá luôn cờ) */

  if (App_LCD_IsReady() == 0U)
  {
    return;
  }

  if (sd_count == 0U)
  {
    /* Chưa có thẻ hoặc chưa có ảnh: thử lại sau mỗi APP_SDCARD_RETRY_MS */
    sd_back_ready = 0U;
    sd_need_preload = 0U;
    if ((HAL_GetTick() - sd_retry_ms) >= APP_SDCARD_RETRY_MS)
    {
      sd_retry_ms = HAL_GetTick();
      if ((sd_mount_and_scan() != 0U) && (sd_decode_next_to_back() != 0U))
      {
        sd_present_back();
      }
    }
    return;
  }

  if ((tap != 0U) && (sd_count > 1U))
  {
    /* Chạm: ảnh kế tiếp đã được tải sẵn thì chỉ cần đổi buffer (tức thì) */
    if ((sd_back_ready != 0U) || (sd_decode_next_to_back() != 0U))
    {
      sd_present_back();
    }
    (void)App_TOUCH_GetTap();  /* bỏ các lần chạm xảy ra trong lúc đang giải mã */
    return;                    /* tải sẵn ở lần gọi sau, để task touch kịp chạy */
  }

  if ((sd_need_preload != 0U) && (sd_count > 1U))
  {
    /* Tải sẵn ảnh kế tiếp vào back buffer trong lúc người dùng đang xem ảnh hiện tại */
    sd_need_preload = 0U;
    sd_back_ready = sd_decode_next_to_back();
  }
}
