/* SPDX-License-Identifier: GPL-3.0-only */
/* Native I1 480x800 renderer. Palette is stripped, never dithered from RGB. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lvgl.h>

int preview_init(void);
int preview_run_assertions(void);
int preview_render_fixture(const char *name, lv_obj_t *screen);

#define WIDTH 480
#define HEIGHT 800
#define STRIDE (WIDTH / 8)
#define PALETTE_BYTES 8
_Static_assert(LV_MEM_SIZE == 64U * 1024U, "Preview requires exactly 64 KiB LVGL heap");
_Static_assert(LV_COLOR_DEPTH == 1, "Preview requires native one-bit color");
static uint8_t draw_buffer[STRIDE * HEIGHT + PALETTE_BYTES]
	__attribute__((aligned(64)));
static uint8_t bitmap[STRIDE * HEIGHT];
static unsigned flushes;

static bool clipped_labels_fit(lv_obj_t *object)
{
	if (lv_obj_check_type(object, &lv_label_class) &&
	    lv_label_get_long_mode(object) == LV_LABEL_LONG_CLIP) {
		lv_point_t size;
		lv_text_get_size(&size, lv_label_get_text(object),
			lv_obj_get_style_text_font(object, LV_PART_MAIN),
			lv_obj_get_style_text_letter_space(object, LV_PART_MAIN),
			lv_obj_get_style_text_line_space(object, LV_PART_MAIN),
			LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		if (size.x > lv_obj_get_content_width(object) ||
		    size.y > lv_obj_get_content_height(object)) {
			fprintf(stderr, "preview: clipped label needs %dx%d, available %dx%d\n",
				(int)size.x, (int)size.y, (int)lv_obj_get_content_width(object),
				(int)lv_obj_get_content_height(object));
			return false;
		}
	}
	for (uint32_t i = 0; i < lv_obj_get_child_count(object); ++i) {
		if (!clipped_labels_fit(lv_obj_get_child(object, i))) return false;
	}
	return true;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
	if (lv_display_get_color_format(display) != LV_COLOR_FORMAT_I1 ||
		area->x1 != 0 || area->y1 != 0 || area->x2 != WIDTH - 1 || area->y2 != HEIGHT - 1 ||
		lv_draw_buf_width_to_stride(WIDTH, LV_COLOR_FORMAT_I1) != STRIDE) {
		fprintf(stderr, "preview: expected full 480x800 I1 flush\n");
		exit(1);
	}
	/* LVGL I1 palette: black index 0 / white index 1. PBM uses black bit 1. */
	pixels += PALETTE_BYTES;
	for (size_t i = 0; i < sizeof(bitmap); ++i) bitmap[i] = ~pixels[i];
	++flushes;
	lv_display_flush_ready(display);
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: preview <fixture> <output.pbm>\n");
		return 2;
	}
	lv_init();
	lv_display_t *display = lv_display_create(WIDTH, HEIGHT);
	if (display == NULL) return 1;
	lv_display_set_color_format(display, LV_COLOR_FORMAT_I1);
	lv_display_set_flush_cb(display, flush);
	lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_FULL);
	lv_display_set_default(display);
	lv_display_set_antialiasing(display, false);
	int ret = preview_init();
	if (ret == 0) {
		ret = preview_run_assertions();
		if (ret != 0) {
			fprintf(stderr, "preview: interaction assertions failed: %d\n", ret);
			return 1;
		}
	}
	if (ret == 0) ret = preview_render_fixture(argv[1], lv_screen_active());
	if (ret < 0) {
		fprintf(stderr, "preview: %s render failed: %d\n", argv[1], ret);
		return 1;
	}
	for (unsigned i = 0; i < 4; ++i) {
		lv_tick_inc(16);
		lv_timer_handler();
	}
	lv_obj_update_layout(lv_screen_active());
	if (!clipped_labels_fit(lv_screen_active())) return 1;
	lv_refr_now(display);
	if (!flushes) {
		fprintf(stderr, "preview: no display flush\n");
		return 1;
	}
	lv_mem_monitor_t memory;
	lv_mem_monitor(&memory);
	fprintf(stderr, "%s: I1 480x800 LVGL budget=65536 allocatable=%zu used=%zu peak=%zu free=%zu\n",
		argv[1], memory.total_size, memory.total_size - memory.free_size,
		memory.max_used, memory.free_size);
	if (memory.total_size > 65536 || memory.max_used > 65536 || lv_mem_test() != LV_RESULT_OK)
		return 1;
	FILE *file = fopen(argv[2], "wb");
	if (file == NULL) {
		fprintf(stderr, "preview: %s: %s\n", argv[2], strerror(errno));
		return 1;
	}
	fprintf(file, "P4\n%d %d\n", WIDTH, HEIGHT);
	if (fwrite(bitmap, 1, sizeof(bitmap), file) != sizeof(bitmap)) return 1;
	if (fclose(file) != 0) return 1;
	lv_deinit();
	return 0;
}
