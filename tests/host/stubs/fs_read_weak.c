/*
 * Weak fs_read for host unit tests that link modules using
 * papermono_fs_read_chunked() but do not exercise real filesystem IO.
 * Tests that simulate the filesystem provide their own strong definition.
 */
#include <sys/types.h>

#include <zephyr/fs/fs.h>

__attribute__((weak)) ssize_t fs_read(struct fs_file_t *file, void *buffer,
				      size_t length)
{
	(void)file;
	(void)buffer;
	(void)length;
	return -1;
}
