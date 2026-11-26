/*
 * App_SDRAM.c
 *
 * Chuỗi khởi tạo SDRAM IS42S16400 qua FMC:
 * clock enable -> chờ >=100 us -> precharge all -> 8 auto-refresh
 * -> load mode register (BL=1, CL=3, single write) -> đặt refresh rate.
 */

#include "App_SDRAM.h"
#include "main.h"

extern SDRAM_HandleTypeDef hsdram1;

#define SDRAM_TIMEOUT                    0xFFFFU

/* Thanh ghi mode của SDRAM */
#define SDRAM_MODEREG_BURST_LENGTH_1     0x0000U
#define SDRAM_MODEREG_BURST_TYPE_SEQ     0x0000U
#define SDRAM_MODEREG_CAS_LATENCY_3      0x0030U
#define SDRAM_MODEREG_OPERATING_STANDARD 0x0000U
#define SDRAM_MODEREG_WRITEBURST_SINGLE  0x0200U

/* Refresh: 64 ms / 4096 hàng = 15.625 us; SDCLK 100 MHz -> 1562 - 20 = 1542 */
#define SDRAM_REFRESH_COUNT              1542U

static uint32_t sdram_send(uint32_t mode, uint32_t autorefresh, uint32_t modereg)
{
  FMC_SDRAM_CommandTypeDef cmd = {0};

  cmd.CommandMode            = mode;
  cmd.CommandTarget          = FMC_SDRAM_CMD_TARGET_BANK1;
  cmd.AutoRefreshNumber      = autorefresh;
  cmd.ModeRegisterDefinition = modereg;

  return (HAL_SDRAM_SendCommand(&hsdram1, &cmd, SDRAM_TIMEOUT) == HAL_OK) ? 0U : 1U;
}

uint32_t App_SDRAM_Init(void)
{
  uint32_t err = 0U;

  err |= sdram_send(FMC_SDRAM_CMD_CLK_ENABLE, 1U, 0U);
  HAL_Delay(1);   /* tối thiểu 100 us */
  err |= sdram_send(FMC_SDRAM_CMD_PALL, 1U, 0U);
  err |= sdram_send(FMC_SDRAM_CMD_AUTOREFRESH_MODE, 8U, 0U);
  err |= sdram_send(FMC_SDRAM_CMD_LOAD_MODE, 1U,
                    SDRAM_MODEREG_BURST_LENGTH_1 | SDRAM_MODEREG_BURST_TYPE_SEQ |
                    SDRAM_MODEREG_CAS_LATENCY_3  | SDRAM_MODEREG_OPERATING_STANDARD |
                    SDRAM_MODEREG_WRITEBURST_SINGLE);

  if (HAL_SDRAM_ProgramRefreshRate(&hsdram1, SDRAM_REFRESH_COUNT) != HAL_OK)
  {
    err |= 1U;
  }

  return err;
}

/* Biến trong Core/Src/sysmem.c (_sbrk) */
extern uint8_t *volatile g_sdram_heap_start;
extern uint8_t *volatile g_sdram_heap_limit;

void App_SDRAM_EnableHeap(void)
{
  g_sdram_heap_limit = (uint8_t *)APP_SDRAM_HEAP_END;
  g_sdram_heap_start = (uint8_t *)APP_SDRAM_HEAP_START;
}
