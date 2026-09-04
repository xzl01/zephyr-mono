/* SPDX-License-Identifier: MIT */

#ifndef PAPERMONO_EPUB_H_
#define PAPERMONO_EPUB_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <papermono/content_store.h>
#include <papermono/zip_reader.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAPERMONO_EPUB_SPINE_LIMIT 64U
#define PAPERMONO_EPUB_TOC_LIMIT 128U
#define PAPERMONO_EPUB_TITLE_MAX 127U

struct papermono_epub_chapter {
	char path[PAPERMONO_ZIP_NAME_MAX + 1U];
	char title[PAPERMONO_EPUB_TITLE_MAX + 1U];
	/* Inflated XHTML size, in the same bytes as text_start/text_end. Filled
	 * in by papermono_epub_read_page from the zip lookup it already does, so
	 * learning it costs no extra I/O. Zero means not yet known.
	 */
	uint32_t size;
};

struct papermono_epub_toc_entry {
	char title[PAPERMONO_EPUB_TITLE_MAX + 1U];
	uint16_t chapter;
	uint8_t level;
};

struct papermono_epub_book {
	struct papermono_zip_archive archive;
	char title[PAPERMONO_EPUB_TITLE_MAX + 1U];
	struct papermono_epub_chapter chapters[PAPERMONO_EPUB_SPINE_LIMIT];
	struct papermono_epub_toc_entry toc[PAPERMONO_EPUB_TOC_LIMIT];
	uint16_t chapter_count;
	uint16_t toc_count;
};

struct papermono_epub_page {
	uint32_t text_start;
	uint32_t text_end;
	bool chapter_eof;
};

int papermono_epub_open(struct papermono_epub_book *epub,
	const struct papermono_content_entry *source);
int papermono_epub_read_page(struct papermono_epub_book *epub,
	uint16_t chapter, uint32_t text_start, char *output, size_t output_size,
	uint8_t columns, uint8_t lines, struct papermono_epub_page *page);

#ifdef __cplusplus
}
#endif

#endif /* PAPERMONO_EPUB_H_ */
