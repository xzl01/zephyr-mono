/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PAPERMONO_RSS_TABLE_H_
#define PAPERMONO_RSS_TABLE_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Versioned, line-oriented records embedded in otherwise ordinary UTF-8 text.
 * HTML-originated control characters are removed before these are emitted.
 * Cells contain neither tabs nor newlines; those delimit columns and rows.
 */
#define PAPERMONO_RSS_TABLE_BEGIN "\x1e" "T1\n"
#define PAPERMONO_RSS_TABLE_HEADER "\x1e" "H\t"
#define PAPERMONO_RSS_TABLE_ROW "\x1e" "R\t"
#define PAPERMONO_RSS_TABLE_END "\x1e" "E\n"

struct papermono_rss_slice {
	const char *text;
	size_t length;
};

struct papermono_rss_table {
	/* Encoded row records, excluding the opening/closing records. */
	struct papermono_rss_slice rows;
	size_t row_count;
	size_t column_count;
	size_t header_rows;
	/* False when the body ended before the closing record. Valid rows remain
	 * readable, including a final partial row, without reading past the body.
	 */
	bool complete;
};

enum papermono_rss_block_kind {
	PAPERMONO_RSS_BLOCK_TEXT,
	PAPERMONO_RSS_BLOCK_TABLE,
};

struct papermono_rss_block {
	enum papermono_rss_block_kind kind;
	struct papermono_rss_slice text;
	struct papermono_rss_table table;
};

struct papermono_rss_table_row {
	/* Tab-separated cell text, excluding its row marker and newline. */
	struct papermono_rss_slice cells;
	size_t column_count;
	bool header;
};

/* All iterators are zero-copy views into the supplied, length-bounded body.
 * Start each cursor at zero. No allocation, column limit or text truncation.
 * A cell slice is not necessarily NUL terminated. Empty cells are significant.
 * Unknown control records are not trusted as tables; old plain bodies work.
 */
bool papermono_rss_block_next(const char *body, size_t length, size_t *cursor,
			     struct papermono_rss_block *block);
bool papermono_rss_table_row_next(const struct papermono_rss_table *table,
				 size_t *cursor,
				 struct papermono_rss_table_row *row);
bool papermono_rss_table_cell_next(const struct papermono_rss_table_row *row,
				  size_t *cursor,
				  struct papermono_rss_slice *cell);

#ifdef __cplusplus
}
#endif

#endif
