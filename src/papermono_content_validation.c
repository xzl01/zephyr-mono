/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/fs/fs.h>
#include <zephyr/sys/util.h>

#include <papermono/content_validation.h>
#include <papermono/epub.h>
#include <papermono/personal_data.h>
#include <papermono/text_codec.h>

#if defined(CONFIG_ESP_SPIRAM)
#define PAPERMONO_VALIDATION_BSS __attribute__((section(".ext_ram.bss")))
#else
#define PAPERMONO_VALIDATION_BSS
#endif

static char validation_buffer[PAPERMONO_METADATA_FILE_LIMIT + 1U]
	PAPERMONO_VALIDATION_BSS;
static struct papermono_calendar validation_calendar PAPERMONO_VALIDATION_BSS;
static struct papermono_task_list validation_tasks PAPERMONO_VALIDATION_BSS;
static struct papermono_note validation_note PAPERMONO_VALIDATION_BSS;
static struct papermono_weread_catalog validation_weread
	PAPERMONO_VALIDATION_BSS;
static struct papermono_epub_book validation_epub PAPERMONO_VALIDATION_BSS;

static int read_file(const char *path, uint64_t size)
{
	struct fs_file_t file;
	ssize_t received;

	if (size > PAPERMONO_METADATA_FILE_LIMIT) {
		return -EFBIG;
	}
	fs_file_t_init(&file);
	int ret = fs_open(&file, path, FS_O_READ);
	if (ret < 0) {
		return ret;
	}
	received = fs_read(&file, validation_buffer, (size_t)size);
	(void)fs_close(&file);
	if (received < 0) {
		return (int)received;
	}
	if ((uint64_t)received != size) {
		return -EIO;
	}
	validation_buffer[received] = '\0';
	return 0;
}

static int validate_txt(const char *path, uint64_t size)
{
	struct fs_file_t file;
	uint8_t sample[4096];
	ssize_t received;

	fs_file_t_init(&file);
	int ret = fs_open(&file, path, FS_O_READ);
	if (ret < 0) {
		return ret;
	}
	received = fs_read(&file, sample, MIN(size, sizeof(sample)));
	(void)fs_close(&file);
	if (received < 0) {
		return (int)received;
	}
	return received == 0 || papermono_text_detect(sample, (size_t)received,
		NULL) != PAPERMONO_TEXT_ENCODING_UNKNOWN ? 0 : -EILSEQ;
}

int papermono_content_validate(enum papermono_content_kind kind,
	const char *path, uint64_t size)
{
	if (path == NULL) {
		return -EINVAL;
	}
	if (kind == PAPERMONO_CONTENT_WEREAD &&
	    size > PAPERMONO_WEREAD_FILE_LIMIT) {
		return -EFBIG;
	}
	if (kind == PAPERMONO_CONTENT_TXT) {
		return validate_txt(path, size);
	}
	if (kind == PAPERMONO_CONTENT_EPUB) {
		struct papermono_content_entry source = {
			.kind = PAPERMONO_CONTENT_EPUB,
			.size = size,
		};
		(void)snprintf(source.path, sizeof(source.path), "%s", path);
		return papermono_epub_open(&validation_epub, &source);
	}
	int ret = read_file(path, size);
	if (ret < 0) {
		return ret;
	}
	if (!papermono_text_utf8_valid((const uint8_t *)validation_buffer,
		(size_t)size, false)) {
		return -EILSEQ;
	}
	switch (kind) {
	case PAPERMONO_CONTENT_ICS:
		return papermono_parse_ics(validation_buffer, (size_t)size,
			&validation_calendar, false);
	case PAPERMONO_CONTENT_TASKS:
		return papermono_parse_tasks_json(validation_buffer, (size_t)size,
			&validation_tasks);
	case PAPERMONO_CONTENT_NOTE:
		return papermono_parse_markdown(validation_buffer, (size_t)size,
			&validation_note);
	case PAPERMONO_CONTENT_WEREAD:
		return papermono_parse_weread_json(validation_buffer, (size_t)size,
			&validation_weread);
	default:
		return -ENOTSUP;
	}
}
