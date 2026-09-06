// 时钟字体档位宏门控（编译期选择性加载，省 flash）—— Bebas Neue 等宽高瘦数字字体
//
// 字体：@fontsource/bebas-neue（Google Fonts，SIL OFL 开源）。Bebas Neue 是**高瘦**(condensed)
//     的标题型无衬线字体，且 **数字严格等宽**（0-9 每字符 adv_w 相同）：
//       - 数字等宽意味着「无论显示几点，HH:MM 宽度恒定」，不会因窄数字(如 '1')让时间变窄、
//         两侧留白忽大忽小——保持"始终占满两边"。
//       - 高瘦(condensed)：数字高度明显大于宽度（方框高宽比≈1.77），同屏宽能用更大字号，
//         字更高，呈"长方形"视觉，比方形字体(如 Archivo Black)更高瘦，远看时间更大、更清晰。
//       - 130px 在 240 宽屏：88:88≈232px（两侧仅余 3.8px），字高约 93px，高瘦占满全屏。
//
// 分级规则（DISPLAY_WIDTH = config.h 的屏幕逻辑横向像素，含 SWAP_XY 后的结果）：
//   DISPLAY_WIDTH >= 240px（240×240/240×320） -> 130、60、48（PickClockFont 从大往小选 130）
//   DISPLAY_WIDTH >= 160px（128×160 横屏）    -> 60、48（160 宽可用 152px，60px 档需实测，一般落到 48px）
//   DISPLAY_WIDTH <  160px（128 竖屏等）      -> 48（最小兜底）
// 日期 18px 档任何屏都会用到，始终启用。
//
// 说明：各档均为 lv_font_conv 默认格宽，Bebas Neue 为等宽数字，零字距不重叠。
#ifndef CLOCK_FONTS_CONFIG_H
#define CLOCK_FONTS_CONFIG_H

#include "config.h"

// 兜底：若 DISPLAY_WIDTH 尚未定义（异常），按最小档处理，避免无档可用导致链接失败
#if !defined(DISPLAY_WIDTH)
#define CLOCK_FONT_SEL_DISPLAY_WIDTH 0
#else
#define CLOCK_FONT_SEL_DISPLAY_WIDTH DISPLAY_WIDTH
#endif

#define CLOCK_FONT_ENABLE_48   1  // 最小兜底档，恒启用
#define CLOCK_FONT_ENABLE_60   0
#define CLOCK_FONT_ENABLE_130  0
#define CLOCK_FONT_ENABLE_DATE 1  // 日期小字恒启用

#if CLOCK_FONT_SEL_DISPLAY_WIDTH >= 240
#undef  CLOCK_FONT_ENABLE_60
#undef  CLOCK_FONT_ENABLE_130
#define CLOCK_FONT_ENABLE_60   1
#define CLOCK_FONT_ENABLE_130  1
#elif CLOCK_FONT_SEL_DISPLAY_WIDTH >= 160
#undef  CLOCK_FONT_ENABLE_60
#define CLOCK_FONT_ENABLE_60   1
#endif

#endif  // CLOCK_FONTS_CONFIG_H
