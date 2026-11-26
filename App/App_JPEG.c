/*
 * App_JPEG.c
 *
 * Giải mã JPEG bằng codec phần cứng (xem App_JPEG.h).
 *
 * Thứ tự dữ liệu codec xuất ra (giống ST jpeg_utils): mỗi MCU gồm các khối 8x8 byte,
 * mỗi khối theo hàng: các khối Y (theo hàng, trái -> phải, trên -> dưới), rồi 1 khối Cb,
 * 1 khối Cr. Ví dụ 4:2:0 (MCU 16x16): Y0 Y1 Y2 Y3 Cb Cr = 384 byte.
 * Các MCU ra theo thứ tự quét hàng; MCU ở mép phải/dưới có phần đệm ngoài ảnh.
 */

#include <string.h>

#include "App_JPEG.h"
#include "App_LCD.h"
#ifndef APP_JPEG_HOST_TEST
#include "main.h"
#endif

/* Cột/hàng nguồn ứng với mỗi cột/hàng đích (nearest-neighbor) */
static uint16_t jpg_xs[APP_LCD_WIDTH];
static uint16_t jpg_ys[APP_LCD_HEIGHT];

/* Bảng chuyển màu YCbCr -> RGB (JFIF, hệ số x 65536) */
static int16_t  jpg_cr_r[256], jpg_cb_b[256], jpg_cr_g[256], jpg_cb_g[256];
static uint8_t  jpg_tab_ready;

static void jpg_tables(void)
{
  int32_t i;

  for (i = 0; i < 256; i++)
  {
    int32_t c = i - 128;

    jpg_cr_r[i] = (int16_t)(( 91881 * c + 32768) >> 16);   /* 1.40200 */
    jpg_cb_b[i] = (int16_t)((116130 * c + 32768) >> 16);   /* 1.77200 */
    jpg_cr_g[i] = (int16_t)(( 46802 * c + 32768) >> 16);   /* 0.71414 */
    jpg_cb_g[i] = (int16_t)(( 22554 * c + 32768) >> 16);   /* 0.34414 */
  }
  jpg_tab_ready = 1U;
}

static inline uint32_t jpg_clamp(int32_t v)
{
  return (v < 0) ? 0U : ((v > 255) ? 255U : (uint32_t)v);
}

static inline uint16_t jpg_ycc565(uint32_t y, uint32_t cb, uint32_t cr)
{
  int32_t yy = (int32_t)y;
  uint32_t r = jpg_clamp(yy + jpg_cr_r[cr]);
  uint32_t g = jpg_clamp(yy - jpg_cb_g[cb] - jpg_cr_g[cr]);
  uint32_t b = jpg_clamp(yy + jpg_cb_b[cb]);

  return (uint16_t)(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
}

void App_JPEG_Fit(uint32_t src_w, uint32_t src_h, app_jpeg_fit_t *fit)
{
  uint32_t fw, fh;

  if (src_w == 0U) { src_w = 1U; }
  if (src_h == 0U) { src_h = 1U; }
  if ((src_w * APP_LCD_HEIGHT) >= (src_h * APP_LCD_WIDTH))
  {
    fw = APP_LCD_WIDTH;
    fh = (src_h * APP_LCD_WIDTH) / src_w;
  }
  else
  {
    fh = APP_LCD_HEIGHT;
    fw = (src_w * APP_LCD_HEIGHT) / src_h;
  }
  if (fw == 0U) { fw = 1U; }
  if (fh == 0U) { fh = 1U; }
  fit->src_w = src_w;
  fit->src_h = src_h;
  fit->w  = fw;
  fit->h  = fh;
  fit->x0 = (APP_LCD_WIDTH  - fw) / 2U;
  fit->y0 = (APP_LCD_HEIGHT - fh) / 2U;
}

int32_t App_JPEG_Probe(const uint8_t *p, uint32_t len, uint32_t *w, uint32_t *h, uint32_t *sof)
{
  uint32_t i = 2U;

  *w = 0U;
  *h = 0U;
  *sof = 0U;
  if ((len < 4U) || (p[0] != 0xFFU) || (p[1] != 0xD8U))
  {
    return -1;
  }
  while ((i + 4U) <= len)
  {
    uint32_t m, seg;

    if (p[i] != 0xFFU)
    {
      return -1;
    }
    m = p[i + 1U];
    if (m == 0xFFU)                                   /* byte đệm */
    {
      i++;
      continue;
    }
    if ((m == 0x01U) || ((m >= 0xD0U) && (m <= 0xD8U)))
    {
      i += 2U;                                        /* marker không có độ dài */
      continue;
    }
    if ((m == 0xDAU) || (m == 0xD9U))
    {
      return -1;                                      /* tới dữ liệu ảnh mà chưa thấy SOF */
    }
    seg = ((uint32_t)p[i + 2U] << 8) | p[i + 3U];
    if ((m >= 0xC0U) && (m <= 0xCFU) && (m != 0xC4U) && (m != 0xC8U) && (m != 0xCCU))
    {
      if ((i + 10U) > len)
      {
        return -1;
      }
      *sof = m;
      *h = ((uint32_t)p[i + 5U] << 8) | p[i + 6U];
      *w = ((uint32_t)p[i + 7U] << 8) | p[i + 8U];
      /* SOF0 baseline / SOF1 extended sequential Huffman, 8-bit, 1 hoặc 3 thành phần */
      if (((m == 0xC0U) || (m == 0xC1U)) && (p[i + 4U] == 8U) && (*w != 0U) && (*h != 0U) &&
          ((p[i + 9U] == 1U) || (p[i + 9U] == 3U)))
      {
        return 1;
      }
      return 0;
    }
    i += 2U + seg;
  }
  return -1;
}

uint32_t App_JPEG_CoreSetup(app_jpeg_core_t *c, uint16_t *fb, const app_jpeg_fit_t *fit,
                            uint32_t w, uint32_t h, uint32_t ncomp, uint32_t hsf, uint32_t vsf)
{
  uint32_t i;

  if ((w == 0U) || (h == 0U) || (fit->w == 0U) || (fit->h == 0U) ||
      (fit->w > APP_LCD_WIDTH) || (fit->h > APP_LCD_HEIGHT) ||
      (hsf < 1U) || (hsf > 2U) || (vsf < 1U) || (vsf > 2U) ||
      ((ncomp != 1U) && (ncomp != 3U)))
  {
    return 0U;
  }
  if (jpg_tab_ready == 0U)
  {
    jpg_tables();
  }
  if (ncomp == 1U)
  {
    hsf = 1U;
    vsf = 1U;
  }
  c->fb    = fb;
  c->x0    = fit->x0;
  c->y0    = fit->y0;
  c->fw    = fit->w;
  c->fh    = fit->h;
  c->w     = w;
  c->h     = h;
  c->ncomp = ncomp;
  c->hsf   = hsf;
  c->vsf   = vsf;
  c->ny    = hsf * vsf;
  c->mcu_w = 8U * hsf;
  c->mcu_h = 8U * vsf;
  c->mcu_bytes = (c->ny + ((ncomp == 3U) ? 2U : 0U)) * 64U;
  c->mcols = (w + c->mcu_w - 1U) / c->mcu_w;
  c->mcx   = 0U;
  c->mcy   = 0U;
  for (i = 0U; i < c->fw; i++)
  {
    jpg_xs[i] = (uint16_t)((i * w) / c->fw);
  }
  for (i = 0U; i < c->fh; i++)
  {
    jpg_ys[i] = (uint16_t)((i * h) / c->fh);
  }
  return 1U;
}

void App_JPEG_CorePutMCU(app_jpeg_core_t *c, const uint8_t *mcu)
{
  uint32_t mx = c->mcx * c->mcu_w;
  uint32_t my = c->mcy * c->mcu_h;
  /* Cột/hàng đích có điểm nguồn nằm trong MCU: dx*w/fw thuộc [mx, mx+mcu_w) */
  uint32_t dx0 = ((mx * c->fw) + c->w - 1U) / c->w;
  uint32_t dx1 = (((mx + c->mcu_w) * c->fw) + c->w - 1U) / c->w;
  uint32_t dy0 = ((my * c->fh) + c->h - 1U) / c->h;
  uint32_t dy1 = (((my + c->mcu_h) * c->fh) + c->h - 1U) / c->h;
  uint32_t dy, dx;

  /* MCU kế tiếp */
  c->mcx++;
  if (c->mcx >= c->mcols)
  {
    c->mcx = 0U;
    c->mcy++;
  }

  if (dx1 > c->fw) { dx1 = c->fw; }
  if (dy1 > c->fh) { dy1 = c->fh; }
  if ((dx0 >= dx1) || (dy0 >= dy1))
  {
    return;                                           /* MCU không có điểm nào được hiển thị */
  }

  for (dy = dy0; dy < dy1; dy++)
  {
    uint32_t sy = (uint32_t)jpg_ys[dy] - my;
    uint16_t *dst = c->fb + ((c->y0 + dy) * APP_LCD_WIDTH) + c->x0;
    const uint8_t *yrow = mcu + (((sy >> 3) * c->hsf) * 64U) + ((sy & 7U) * 8U);

    if (c->ncomp == 1U)
    {
      for (dx = dx0; dx < dx1; dx++)
      {
        uint32_t sx = (uint32_t)jpg_xs[dx] - mx;
        uint32_t y  = yrow[sx];
        dst[dx] = (uint16_t)(((y & 0xF8U) << 8) | ((y & 0xFCU) << 3) | (y >> 3));
      }
    }
    else
    {
      const uint8_t *cbrow = mcu + (c->ny * 64U) + ((sy >> (c->vsf - 1U)) * 8U);
      const uint8_t *crrow = cbrow + 64U;
      uint32_t hs = c->hsf - 1U;

      for (dx = dx0; dx < dx1; dx++)
      {
        uint32_t sx = (uint32_t)jpg_xs[dx] - mx;
        uint32_t y  = yrow[((sx >> 3) * 64U) + (sx & 7U)];
        uint32_t cx = sx >> hs;
        dst[dx] = jpg_ycc565(y, cbrow[cx], crrow[cx]);
      }
    }
  }
}

#ifndef APP_JPEG_HOST_TEST
/* ===========================================================================
 * Điều khiển codec phần cứng
 * ======================================================================== */
#define JPG_MCU_MAX   (6U * 64U)                      /* 4:2:0: 4 Y + Cb + Cr */

static __attribute__((aligned(4))) uint8_t jpg_mcu[JPG_MCU_MAX];

volatile uint32_t g_jpeg_hw_sr;                       /* JPEG->SR lần lỗi gần nhất */

static void jpg_stop(void)
{
  JPEG->CONFR0 = 0U;                                  /* dừng codec */
  JPEG->CR = JPEG_CR_JCEN;                            /* tắt mọi ngắt, giữ codec bật */
  JPEG->CR |= JPEG_CR_IFF | JPEG_CR_OFF;              /* xả FIFO */
  JPEG->CFR = JPEG_CFR_CEOCF | JPEG_CFR_CHPDF;
}

uint32_t App_JPEG_DecodeHW(uint8_t *buf, uint32_t len, uint32_t buf_size,
                           app_jpeg_read_t rd, void *ctx,
                           uint16_t *fb, const app_jpeg_fit_t *fit, app_jpeg_info_t *info)
{
  static app_jpeg_core_t core;
  const uint32_t *in;
  uint32_t in_words, in_idx = 0U, mcu_fill = 0U;
  uint32_t t_start, t_prog, rd_ms = 0U;
  uint8_t  eof, hdr = 0U;
  uint32_t ret = APP_JPEG_OK;

  memset(info, 0, sizeof(*info));
  if ((((uint32_t)buf & 3U) != 0U) || ((buf_size & 3U) != 0U) || (len > buf_size))
  {
    return APP_JPEG_UNSUPPORTED;
  }

  /* Đệm 0 cho word cuối nếu len không chia hết cho 4 */
  while (((len & 3U) != 0U) && (len < buf_size)) { buf[len++] = 0U; }
  in       = (const uint32_t *)buf;
  in_words = len / 4U;
  eof      = (len < buf_size) ? 1U : 0U;
  info->in_bytes = len;

  /* Bật clock codec và khởi động giải mã có đọc header */
  SET_BIT(RCC->AHB2ENR, RCC_AHB2ENR_JPEGEN);
  (void)READ_BIT(RCC->AHB2ENR, RCC_AHB2ENR_JPEGEN);
  JPEG->CONFR0 = 0U;
  JPEG->CR     = JPEG_CR_JCEN;
  JPEG->CR    |= JPEG_CR_IFF | JPEG_CR_OFF;
  JPEG->CFR    = JPEG_CFR_CEOCF | JPEG_CFR_CHPDF;
  JPEG->CONFR1 = JPEG_CONFR1_DE | JPEG_CONFR1_HDR;
  JPEG->CONFR0 = JPEG_CONFR0_START;

  t_start = HAL_GetTick();
  t_prog  = t_start;
  for (;;)
  {
    uint32_t sr = JPEG->SR;

    /* --- Nạp dữ liệu vào --- */
    if ((in_idx >= in_words) && (eof == 0U))
    {
      uint32_t t0 = HAL_GetTick();
      uint32_t n  = rd(ctx, buf, buf_size);

      rd_ms += HAL_GetTick() - t0;
      if (n == 0xFFFFFFFFU)
      {
        ret = APP_JPEG_READ_ERR;
        break;
      }
      while (((n & 3U) != 0U) && (n < buf_size)) { buf[n++] = 0U; }
      in_words = n / 4U;
      in_idx   = 0U;
      eof      = (n < buf_size) ? 1U : 0U;
      info->in_bytes += n;
      t_prog = HAL_GetTick();
    }
    if (in_idx < in_words)
    {
      if ((sr & JPEG_SR_IFTF) != 0U)
      {
        /* FIFO vào dưới ngưỡng: còn chỗ ít nhất 4 word */
        uint32_t k = in_words - in_idx;
        if (k > 4U) { k = 4U; }
        while (k-- > 0U) { JPEG->DIR = in[in_idx++]; }
        t_prog = HAL_GetTick();
      }
      else if ((sr & JPEG_SR_IFNFF) != 0U)
      {
        JPEG->DIR = in[in_idx++];
        t_prog = HAL_GetTick();
      }
    }

    /* --- Header đã đọc xong: lấy thông tin ảnh --- */
    if ((hdr == 0U) && ((sr & JPEG_SR_HPDF) != 0U))
    {
      uint32_t nf = (JPEG->CONFR1 & JPEG_CONFR1_NF) >> JPEG_CONFR1_NF_Pos;
      uint32_t w  = (JPEG->CONFR3 & JPEG_CONFR3_XSIZE) >> JPEG_CONFR3_XSIZE_Pos;
      uint32_t h  = (JPEG->CONFR1 & JPEG_CONFR1_YSIZE) >> JPEG_CONFR1_YSIZE_Pos;
      uint32_t ncomp = (nf == 0U) ? 1U : ((nf == 2U) ? 3U : 0U);
      uint32_t hsf = 1U, vsf = 1U;

      if (ncomp == 3U)
      {
        uint32_t c4 = JPEG->CONFR4;
        uint32_t ny = ((c4 & JPEG_CONFR4_NB) >> JPEG_CONFR4_NB_Pos) + 1U;
        uint32_t nb_cb = ((JPEG->CONFR5 & JPEG_CONFR4_NB) >> JPEG_CONFR4_NB_Pos) + 1U;
        uint32_t nb_cr = ((JPEG->CONFR6 & JPEG_CONFR4_NB) >> JPEG_CONFR4_NB_Pos) + 1U;

        hsf = (c4 & JPEG_CONFR4_HSF) >> JPEG_CONFR4_HSF_Pos;
        vsf = (c4 & JPEG_CONFR4_VSF) >> JPEG_CONFR4_VSF_Pos;
        if ((hsf * vsf) != ny)
        {
          /* Hệ số lấy mẫu không có trong thanh ghi: suy từ số khối Y */
          hsf = (ny >= 2U) ? 2U : 1U;
          vsf = (ny == 4U) ? 2U : 1U;
        }
        if ((nb_cb != 1U) || (nb_cr != 1U) || (ny > 4U))
        {
          ncomp = 0U;                                 /* chroma lấy mẫu lạ: không hỗ trợ */
        }
      }
      info->width  = w;
      info->height = h;
      info->ncomp  = ncomp;
      info->hsf    = hsf;
      info->vsf    = vsf;
      if ((ncomp == 0U) || (w != fit->src_w) || (h != fit->src_h) ||
          (App_JPEG_CoreSetup(&core, fb, fit, w, h, ncomp, hsf, vsf) == 0U))
      {
        ret = APP_JPEG_UNSUPPORTED;
        break;
      }
      JPEG->CFR = JPEG_CFR_CHPDF;
      hdr = 1U;
    }

    /* --- Lấy dữ liệu ra (MCU) --- */
    if (hdr != 0U)
    {
      if ((sr & JPEG_SR_OFTF) != 0U)
      {
        /* FIFO ra trên ngưỡng: có ít nhất 4 word */
        uint32_t k;
        for (k = 0U; k < 4U; k++)
        {
          uint32_t v = JPEG->DOR;
          memcpy(&jpg_mcu[mcu_fill], &v, 4U);
          mcu_fill += 4U;
          if (mcu_fill >= core.mcu_bytes)
          {
            App_JPEG_CorePutMCU(&core, jpg_mcu);
            info->mcus++;
            mcu_fill = 0U;
          }
        }
        t_prog = HAL_GetTick();
      }
      else if ((sr & JPEG_SR_OFNEF) != 0U)
      {
        uint32_t v = JPEG->DOR;
        memcpy(&jpg_mcu[mcu_fill], &v, 4U);
        mcu_fill += 4U;
        if (mcu_fill >= core.mcu_bytes)
        {
          App_JPEG_CorePutMCU(&core, jpg_mcu);
          info->mcus++;
          mcu_fill = 0U;
        }
        t_prog = HAL_GetTick();
      }
      else if ((sr & JPEG_SR_EOCF) != 0U)
      {
        break;                                        /* xong và FIFO ra đã trống */
      }
    }

    if ((HAL_GetTick() - t_prog) > APP_JPEG_STALL_MS)
    {
      g_jpeg_hw_sr = sr;
      ret = APP_JPEG_TIMEOUT;
      break;
    }
  }

  jpg_stop();
  info->read_ms = rd_ms;
  info->ms = (HAL_GetTick() - t_start) - rd_ms;
  return ret;
}
#endif /* APP_JPEG_HOST_TEST */
