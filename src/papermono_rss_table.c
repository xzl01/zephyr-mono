/* SPDX-License-Identifier: GPL-3.0-only */
#include <papermono/rss_table.h>

#include <stdint.h>
#include <string.h>

static bool marker_at(const char *body, size_t length, size_t offset,
		      const char *marker, size_t marker_length)
{
	return offset <= length && marker_length <= length - offset &&
	       (offset == 0U || body[offset - 1U] == '\n') &&
	       memcmp(body + offset, marker, marker_length) == 0;
}

static bool row_at(const char *body, size_t length, size_t offset,
		   struct papermono_rss_table_row *row, size_t *next)
{
	if (offset > length || length - offset < 3U ||
	    body[offset] != '\x1e' || body[offset + 2U] != '\t' ||
	    (body[offset + 1U] != 'H' && body[offset + 1U] != 'R')) {
		return false;
	}
	const size_t start = offset + 3U;
	size_t end = start;
	size_t columns = 1U;

	while (end < length && body[end] != '\n') {
		if (body[end] == '\t') {
			++columns;
		}
		++end;
	}
	row->cells = (struct papermono_rss_slice) { body + start, end - start };
	row->column_count = columns;
	row->header = body[offset + 1U] == 'H';
	*next = end < length ? end + 1U : end;
	return true;
}

bool papermono_rss_table_row_next(const struct papermono_rss_table *table,
				 size_t *cursor,
				 struct papermono_rss_table_row *row)
{
	if (table == NULL || cursor == NULL || row == NULL ||
	    table->rows.text == NULL || table->rows.length == SIZE_MAX ||
	    *cursor >= table->rows.length) {
		return false;
	}
	size_t next;

	if (!row_at(table->rows.text, table->rows.length, *cursor, row, &next)) {
		*cursor = table->rows.length;
		return false;
	}
	*cursor = next;
	return true;
}

bool papermono_rss_table_cell_next(const struct papermono_rss_table_row *row,
				  size_t *cursor,
				  struct papermono_rss_slice *cell)
{
	if (row == NULL || cursor == NULL || cell == NULL ||
	    row->cells.text == NULL || row->cells.length == SIZE_MAX ||
	    *cursor > row->cells.length) {
		return false;
	}
	const size_t start = *cursor;
	size_t end = start;

	while (end < row->cells.length && row->cells.text[end] != '\t') {
		++end;
	}
	*cell = (struct papermono_rss_slice) { row->cells.text + start, end - start };
	/* length+1 is the exhausted state, preserving an empty final cell. */
	*cursor = end + 1U;
	return true;
}

bool papermono_rss_block_next(const char *body, size_t length, size_t *cursor,
			     struct papermono_rss_block *block)
{
	if (body == NULL || cursor == NULL || block == NULL ||
	    length == SIZE_MAX || *cursor > length) {
		return false;
	}
	while (*cursor < length) {
		const size_t start = *cursor;

		if (marker_at(body, length, start, PAPERMONO_RSS_TABLE_BEGIN, 4U)) {
			const size_t rows_start = start + 4U;
			size_t position = rows_start;
			size_t rows_end = position;
			struct papermono_rss_table table = {0};
			bool header_prefix = true;

			while (position < length) {
				struct papermono_rss_table_row row;
				size_t next;

				if (marker_at(body, length, position,
					      PAPERMONO_RSS_TABLE_END, 3U)) {
					table.complete = true;
					position += 3U;
					break;
				}
				/* A closing record can itself be cut by the body limit. */
				if (length - position == 2U &&
				    body[position] == '\x1e' && body[position + 1U] == 'E') {
					position = length;
					break;
				}
				if (!row_at(body, length, position, &row, &next)) {
					break;
				}
				++table.row_count;
				if (row.column_count > table.column_count) {
					table.column_count = row.column_count;
				}
				if (row.header && header_prefix) {
					++table.header_rows;
				} else {
					header_prefix = false;
				}
				position = next;
				rows_end = next;
			}
			*cursor = position;
			if (table.row_count > 0U) {
				table.rows = (struct papermono_rss_slice) {
					body + rows_start, rows_end - rows_start,
				};
				memset(block, 0, sizeof(*block));
				block->kind = PAPERMONO_RSS_BLOCK_TABLE;
				block->text = (struct papermono_rss_slice) {
					body + start, position - start,
				};
				block->table = table;
				return true;
			}
			/* Empty or malformed opening records never hide following text. */
			continue;
		}
		if (body[start] == '\x1e') {
			++*cursor;
			continue;
		}
		size_t end = start + 1U;

		while (end < length && body[end] != '\x1e') {
			++end;
		}
		memset(block, 0, sizeof(*block));
		block->kind = PAPERMONO_RSS_BLOCK_TEXT;
		block->text = (struct papermono_rss_slice) { body + start, end - start };
		*cursor = end;
		return true;
	}
	return false;
}
