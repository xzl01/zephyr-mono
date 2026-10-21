/* SPDX-License-Identifier: GPL-3.0-only */

/**
 * @file lv_conf.h
 * Host-side preview configuration for the PAPER MONO 480x800 1-bit UI.
 *
 * Only overrides live here; every other option falls back to the LVGL 9.x
 * defaults in lv_conf_internal.h.
 */

#ifndef PAPERMONO_PREVIEW_LV_CONF_H_
#define PAPERMONO_PREVIEW_LV_CONF_H_

/* LVGL checks for its own guard name to confirm the file was really used. */
#define LV_CONF_H 1

/* The panel is a 1-bit e-paper framebuffer, so render in 1 bpp to match. */
#define LV_COLOR_DEPTH 1

#define LV_USE_OS LV_OS_NONE

#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (64U * 1024U)

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1

#define LV_DEF_REFR_PERIOD 33
#define LV_DPI_DEF 130

/* Fonts used by src/papermono_ui.c */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_USE_FONT_PLACEHOLDER 1
#define LV_FONT_FMT_TXT_LARGE 1

/* Widgets referenced by the UI, plus their dependencies. */
#define LV_USE_LABEL 1
#define LV_USE_IMAGE 1
#define LV_USE_LINE 1
#define LV_USE_BUTTON 1
#define LV_USE_CANVAS 1
#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/* Software renderer: only the 1 bpp path is exercised. */
#define LV_USE_DRAW_SW 1
#define LV_DRAW_SW_SUPPORT_I1 1
#define LV_DRAW_SW_SUPPORT_L8 1
#define LV_DRAW_SW_SUPPORT_A8 1
#define LV_DRAW_SW_SUPPORT_RGB565 0
#define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED 0
#define LV_DRAW_SW_SUPPORT_RGB565A8 0
#define LV_DRAW_SW_SUPPORT_RGB888 0
#define LV_DRAW_SW_SUPPORT_XRGB8888 0
#define LV_DRAW_SW_SUPPORT_ARGB8888 0
#define LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED 0
#define LV_DRAW_SW_SUPPORT_AL88 0
#define LV_DRAW_SW_SUPPORT_I1 1
#define LV_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE
#define LV_USE_DRAW_SW_COMPLEX_GRADIENTS 0

/* No accelerators, no vector graphics, no extra libs. */
#define LV_USE_DRAW_PXP 0
#define LV_USE_DRAW_G2D 0
#define LV_USE_DRAW_VG_LITE 0
#define LV_USE_DRAW_OPENGLES 0
#define LV_USE_NEMA_GFX 0
#define LV_USE_VECTOR_GRAPHIC 0
#define LV_USE_MATRIX 0
#define LV_USE_THORVG_INTERNAL 0
#define LV_USE_THORVG_EXTERNAL 0
#define LV_USE_FREETYPE 0
#define LV_USE_TINY_TTF 0
#define LV_USE_SVG 0
#define LV_USE_LODEPNG 0
#define LV_USE_LIBPNG 0
#define LV_USE_BMP 0
#define LV_USE_TJPGD 0
#define LV_USE_GIF 0
#define LV_USE_QRCODE 0
#define LV_USE_BARCODE 0

/* No filesystem, no widgets/demos/examples we do not render. */
#define LV_USE_FS_STDIO 0
#define LV_USE_FS_POSIX 0
#define LV_USE_FS_MEMFS 0
#define LV_USE_OBSERVER 0
#define LV_USE_SNAPSHOT 0
#define LV_USE_TRANSLATION 0
#define LV_USE_OBJ_ID 0
#define LV_USE_OBJ_PROPERTY 0

#endif /* PAPERMONO_PREVIEW_LV_CONF_H_ */
