/*
 * App_font8x16.h
 *
 * Font bitmap 8x16, ASCII 32..126.
 */

#ifndef APP_APP_FONT8X16_H_
#define APP_APP_FONT8X16_H_

#include <stdint.h>

#define APP_FONT_W      8U
#define APP_FONT_H      16U
#define APP_FONT_FIRST  32U
#define APP_FONT_LAST   126U

extern const uint8_t g_font8x16[95][16];

#endif /* APP_APP_FONT8X16_H_ */
