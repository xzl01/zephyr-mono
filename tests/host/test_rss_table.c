/* SPDX-License-Identifier: GPL-3.0-only */
#include <papermono/rss_table.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void equals(struct papermono_rss_slice slice, const char *expected)
{
	assert(slice.length == strlen(expected));
	assert(memcmp(slice.text, expected, slice.length) == 0);
}

static void test_mixed_blocks(void)
{
	static const char body[] = "前文\n\n" PAPERMONO_RSS_TABLE_BEGIN
		PAPERMONO_RSS_TABLE_HEADER "品种\t现价\t\n"
		PAPERMONO_RSS_TABLE_ROW "黄金\t1234.56\t说明\n"
		PAPERMONO_RSS_TABLE_ROW "\t\t\n"
		PAPERMONO_RSS_TABLE_END "\n后文";
	struct papermono_rss_block block;
	size_t cursor = 0U;

	assert(papermono_rss_block_next(body, sizeof(body) - 1U, &cursor, &block));
	assert(block.kind == PAPERMONO_RSS_BLOCK_TEXT);
	equals(block.text, "前文\n\n");
	assert(papermono_rss_block_next(body, sizeof(body) - 1U, &cursor, &block));
	assert(block.kind == PAPERMONO_RSS_BLOCK_TABLE);
	assert(block.table.row_count == 3U && block.table.column_count == 3U);
	assert(block.table.header_rows == 1U && block.table.complete);
	struct papermono_rss_table_row row;
	size_t rows = 0U;
	assert(papermono_rss_table_row_next(&block.table, &rows, &row));
	assert(row.header && row.column_count == 3U);
	size_t cells = 0U;
	struct papermono_rss_slice cell;
	assert(papermono_rss_table_cell_next(&row, &cells, &cell)); equals(cell, "品种");
	assert(papermono_rss_table_cell_next(&row, &cells, &cell)); equals(cell, "现价");
	assert(papermono_rss_table_cell_next(&row, &cells, &cell)); equals(cell, "");
	assert(!papermono_rss_table_cell_next(&row, &cells, &cell));
	assert(papermono_rss_table_row_next(&block.table, &rows, &row));
	assert(!row.header && row.column_count == 3U);
	cells = 0U;
	assert(papermono_rss_table_cell_next(&row, &cells, &cell)); equals(cell, "黄金");
	assert(papermono_rss_table_cell_next(&row, &cells, &cell)); equals(cell, "1234.56");
	assert(papermono_rss_table_cell_next(&row, &cells, &cell)); equals(cell, "说明");
	assert(!papermono_rss_table_cell_next(&row, &cells, &cell));
	assert(papermono_rss_table_row_next(&block.table, &rows, &row));
	assert(!row.header && row.column_count == 3U);
	cells = 0U;
	for (size_t i = 0U; i < 3U; ++i) {
		assert(papermono_rss_table_cell_next(&row, &cells, &cell)); equals(cell, "");
	}
	assert(!papermono_rss_table_cell_next(&row, &cells, &cell));
	assert(!papermono_rss_table_row_next(&block.table, &rows, &row));
	assert(papermono_rss_block_next(body, sizeof(body) - 1U, &cursor, &block));
	assert(block.kind == PAPERMONO_RSS_BLOCK_TEXT); equals(block.text, "\n后文");
	assert(!papermono_rss_block_next(body, sizeof(body) - 1U, &cursor, &block));
}

static void test_legacy_and_multiple_tables(void)
{
	static const char plain[] = "old cached text\n\n第二段\n· list";
	struct papermono_rss_block block;
	size_t cursor = 0U;
	assert(papermono_rss_block_next(plain, sizeof(plain) - 1U, &cursor, &block));
	assert(block.kind == PAPERMONO_RSS_BLOCK_TEXT); equals(block.text, plain);
	assert(!papermono_rss_block_next(plain, sizeof(plain) - 1U, &cursor, &block));
	static const char tables[] = PAPERMONO_RSS_TABLE_BEGIN PAPERMONO_RSS_TABLE_END
		"caption\n" PAPERMONO_RSS_TABLE_BEGIN
		PAPERMONO_RSS_TABLE_HEADER "first\n" PAPERMONO_RSS_TABLE_HEADER "second\n"
		PAPERMONO_RSS_TABLE_ROW "data\n" PAPERMONO_RSS_TABLE_END
		PAPERMONO_RSS_TABLE_BEGIN PAPERMONO_RSS_TABLE_ROW "last\n" PAPERMONO_RSS_TABLE_END;
	cursor = 0U;
	assert(papermono_rss_block_next(tables, sizeof(tables) - 1U, &cursor, &block));
	assert(block.kind == PAPERMONO_RSS_BLOCK_TEXT); equals(block.text, "caption\n");
	assert(papermono_rss_block_next(tables, sizeof(tables) - 1U, &cursor, &block));
	assert(block.kind == PAPERMONO_RSS_BLOCK_TABLE && block.table.header_rows == 2U);
	assert(block.table.row_count == 3U && block.table.column_count == 1U);
	assert(papermono_rss_block_next(tables, sizeof(tables) - 1U, &cursor, &block));
	assert(block.kind == PAPERMONO_RSS_BLOCK_TABLE && block.table.row_count == 1U);
	assert(!papermono_rss_block_next(tables, sizeof(tables) - 1U, &cursor, &block));
}

static void bounds(const char *body, size_t length)
{
	struct papermono_rss_block block;
	size_t cursor = 0U;
	size_t iterations = 0U;

	while (papermono_rss_block_next(body, length, &cursor, &block)) {
		assert(++iterations <= length + 1U && cursor <= length);
		assert(block.text.text >= body && block.text.text <= body + length);
		assert(block.text.length <= (size_t)(body + length - block.text.text));
		if (block.kind != PAPERMONO_RSS_BLOCK_TABLE) continue;
		struct papermono_rss_table_row row;
		size_t rows = 0U;
		size_t row_count = 0U;
		while (papermono_rss_table_row_next(&block.table, &rows, &row)) {
			++row_count;
			assert(row.cells.text >= body && row.cells.text <= body + length);
			assert(row.cells.length <= (size_t)(body + length - row.cells.text));
			size_t cells = 0U;
			size_t count = 0U;
			struct papermono_rss_slice cell;
			while (papermono_rss_table_cell_next(&row, &cells, &cell)) {
				assert(++count <= row.cells.length + 1U);
				assert(cell.text >= row.cells.text);
				assert(cell.text + cell.length <= row.cells.text + row.cells.length);
			}
			assert(count == row.column_count);
		}
		assert(row_count == block.table.row_count);
	}
}

static void test_truncation_and_malformed(void)
{
	static const char body[] = "before\n" PAPERMONO_RSS_TABLE_BEGIN
		PAPERMONO_RSS_TABLE_HEADER "H1\tH2\n" PAPERMONO_RSS_TABLE_ROW "长格\t\n"
		PAPERMONO_RSS_TABLE_END "after";
	for (size_t cut = 0U; cut < sizeof(body); ++cut) bounds(body, cut);
	static const char partial[] = PAPERMONO_RSS_TABLE_BEGIN PAPERMONO_RSS_TABLE_ROW "tail";
	struct papermono_rss_block block;
	size_t cursor = 0U;
	assert(papermono_rss_block_next(partial, sizeof(partial) - 1U, &cursor, &block));
	assert(block.kind == PAPERMONO_RSS_BLOCK_TABLE && !block.table.complete);
	assert(block.table.row_count == 1U);
	static const char malformed[] = PAPERMONO_RSS_TABLE_BEGIN
		PAPERMONO_RSS_TABLE_ROW "kept\n" "not a record\nafter";
	cursor = 0U;
	assert(papermono_rss_block_next(malformed, sizeof(malformed) - 1U, &cursor, &block));
	assert(block.kind == PAPERMONO_RSS_BLOCK_TABLE && !block.table.complete);
	assert(papermono_rss_block_next(malformed, sizeof(malformed) - 1U, &cursor, &block));
	equals(block.text, "not a record\nafter");
	bounds("x\x1e" "T1\nnot a table", 17U);
	assert(!papermono_rss_block_next(NULL, 1U, &cursor, &block));
	assert(!papermono_rss_block_next(body, SIZE_MAX, &cursor, &block));
	assert(!papermono_rss_block_next(body, 1U, NULL, &block));
}

static void test_wide_long_and_random(void)
{
	char body[4096];
	size_t length = 0U;
	memcpy(body + length, PAPERMONO_RSS_TABLE_BEGIN PAPERMONO_RSS_TABLE_ROW, 7U); length += 7U;
	for (size_t i = 0U; i < 40U; ++i) {
		if (i > 0U) body[length++] = '\t';
		body[length++] = 'x';
	}
	body[length++] = '\n';
	memcpy(body + length, PAPERMONO_RSS_TABLE_ROW, 3U); length += 3U;
	for (size_t i = 0U; i < 180U; ++i) { memcpy(body + length, "长", 3U); length += 3U; }
	body[length++] = '\n';
	memcpy(body + length, PAPERMONO_RSS_TABLE_END, 3U); length += 3U;
	struct papermono_rss_block block;
	size_t cursor = 0U;
	assert(papermono_rss_block_next(body, length, &cursor, &block));
	assert(block.table.column_count == 40U && block.table.row_count == 2U);
	size_t rows = 0U;
	struct papermono_rss_table_row row;
	assert(papermono_rss_table_row_next(&block.table, &rows, &row));
	assert(papermono_rss_table_row_next(&block.table, &rows, &row));
	assert(row.cells.length == 540U);
	bounds(body, length);
	uint32_t state = 0x7461626cU;
	for (size_t round = 0U; round < 128U; ++round) {
		for (size_t i = 0U; i < sizeof(body); ++i) {
			state = state * 1664525U + 1013904223U;
			body[i] = (char)(state >> 24U);
		}
		bounds(body, sizeof(body));
	}
}

int main(void)
{
	test_mixed_blocks();
	test_legacy_and_multiple_tables();
	test_truncation_and_malformed();
	test_wide_long_and_random();
	puts("RSS table codec: mixed blocks, zero-copy cells, long/wide data and bounded malformed input: PASS");
	return 0;
}
