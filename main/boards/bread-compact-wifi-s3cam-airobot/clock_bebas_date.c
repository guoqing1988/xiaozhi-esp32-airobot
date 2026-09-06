/*******************************************************************************
 * Size: 18 px
 * Bpp: 2
 * Opts: --size 18 --bpp 2 --format lvgl --no-compress --font node_modules/@fontsource/bebas-neue/files/bebas-neue-latin-400-normal.woff --range 0x30-0x39,0x2D --lv-font-name clock_bebas_date -o D:/data/www/wwwroot/xiaozhi-esp32-airobot/main/boards/bread-compact-wifi-s3cam-airobot/clock_bebas_date.c
 ******************************************************************************/

#include "clock_fonts_config.h"

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef CLOCK_BEBAS_DATE
#define CLOCK_BEBAS_DATE 1
#endif

#if CLOCK_BEBAS_DATE && CLOCK_FONT_ENABLE_DATE

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+002D "-" */
    0x7f, 0x6f, 0xd0,

    /* U+0030 "0" */
    0x1f, 0xd0, 0xff, 0xc7, 0x8b, 0x9e, 0x1e, 0x78,
    0x79, 0xe1, 0xe7, 0x87, 0x9e, 0x1e, 0x78, 0x79,
    0xe1, 0xe7, 0x8b, 0x8f, 0xfc, 0x1f, 0xd0,

    /* U+0031 "1" */
    0x3, 0x43, 0xdb, 0xf4, 0x6d, 0x7, 0x41, 0xd0,
    0x74, 0x1d, 0x7, 0x41, 0xd0, 0x74, 0x1d, 0x7,
    0x40,

    /* U+0032 "2" */
    0xf, 0xd0, 0xff, 0xc3, 0x8b, 0x9e, 0x1e, 0x14,
    0xb4, 0x3, 0xc0, 0x3d, 0x2, 0xe0, 0x1f, 0x0,
    0xf0, 0x3, 0x80, 0x1f, 0xfd, 0x7f, 0xf4,

    /* U+0033 "3" */
    0x1f, 0xd0, 0xff, 0xc7, 0x8b, 0x59, 0x2d, 0x0,
    0xb4, 0x1f, 0x80, 0x7e, 0x0, 0x3d, 0x10, 0xb5,
    0xd1, 0xd7, 0x8b, 0x4f, 0xfc, 0x1f, 0xd0,

    /* U+0034 "4" */
    0x1, 0xf0, 0xb, 0xc0, 0x3f, 0x2, 0xfc, 0xf,
    0xf0, 0x77, 0xc2, 0xcf, 0xe, 0x3c, 0xb4, 0xf2,
    0xff, 0xfb, 0xff, 0xc0, 0x3c, 0x0, 0xf0,

    /* U+0035 "5" */
    0x3f, 0xf4, 0xff, 0xc3, 0xc0, 0xe, 0x0, 0x3a,
    0xe0, 0xff, 0xd7, 0x8b, 0x80, 0x1e, 0x0, 0x79,
    0xe1, 0xe3, 0x8b, 0x4f, 0xfc, 0xf, 0xd0,

    /* U+0036 "6" */
    0xf, 0xd0, 0xff, 0xd7, 0xc7, 0x9e, 0x0, 0x7a,
    0xe1, 0xff, 0xd7, 0xcb, 0x9e, 0x1e, 0x78, 0x79,
    0xe1, 0xe7, 0xc7, 0x8f, 0xfc, 0xf, 0xd0,

    /* U+0037 "7" */
    0x7f, 0xf9, 0xff, 0xe0, 0xb, 0x40, 0x3c, 0x0,
    0xf0, 0x7, 0x80, 0x2c, 0x0, 0xf0, 0x7, 0x80,
    0x2d, 0x0, 0xf0, 0x3, 0xc0, 0x1e, 0x0,

    /* U+0038 "8" */
    0x1f, 0xd0, 0xff, 0xd7, 0x87, 0x9e, 0x1e, 0x78,
    0x74, 0xbf, 0xc2, 0xff, 0x1e, 0x2e, 0x74, 0x79,
    0xd1, 0xe7, 0x87, 0x8f, 0xfd, 0x1f, 0xd0,

    /* U+0039 "9" */
    0x1f, 0xd0, 0xff, 0xc7, 0x8b, 0x5e, 0x1e, 0x78,
    0x79, 0xe2, 0xe3, 0xff, 0x8b, 0xee, 0x0, 0x78,
    0x91, 0xe7, 0x8b, 0x4f, 0xfc, 0x1f, 0xd0
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 78, .box_w = 5, .box_h = 2, .ofs_x = 0, .ofs_y = 6},
    {.bitmap_index = 3, .adv_w = 115, .box_w = 7, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 26, .adv_w = 115, .box_w = 5, .box_h = 13, .ofs_x = 1, .ofs_y = 0},
    {.bitmap_index = 43, .adv_w = 115, .box_w = 7, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 66, .adv_w = 115, .box_w = 7, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 89, .adv_w = 115, .box_w = 7, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 112, .adv_w = 115, .box_w = 7, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 135, .adv_w = 115, .box_w = 7, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 158, .adv_w = 115, .box_w = 7, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 181, .adv_w = 115, .box_w = 7, .box_h = 13, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 204, .adv_w = 115, .box_w = 7, .box_h = 13, .ofs_x = 0, .ofs_y = 0}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/

static const uint8_t glyph_id_ofs_list_0[] = {
    0, 0, 0, 1, 2, 3, 4, 5,
    6, 7, 8, 9, 10
};

/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 45, .range_length = 13, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = glyph_id_ofs_list_0, .list_length = 13, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL
    }
};



/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = 1,
    .bpp = 2,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t clock_bebas_date = {
#else
lv_font_t clock_bebas_date = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 13,          /*The maximum line height required by the font*/
    .base_line = 0,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = NULL,
#endif
    .user_data = NULL,
};



#endif /*#if CLOCK_BEBAS_DATE*/

