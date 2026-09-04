/* Host-test Zephyr settings shim. */
#ifndef ZEPHYR_SETTINGS_SETTINGS_H_
#define ZEPHYR_SETTINGS_SETTINGS_H_

#include <stddef.h>
#include <string.h>

typedef int (*settings_read_cb)(void *cb_arg, void *data, size_t len);
typedef int (*settings_load_direct_cb)(const char *key, size_t len,
	settings_read_cb read_cb, void *cb_arg, void *param);

static inline int settings_name_steq(const char *name, const char *key,
	const char **next)
{
	const size_t length = strlen(key);

	if (strncmp(name, key, length) != 0 ||
	    (name[length] != '\0' && name[length] != '/')) {
		return 0;
	}
	if (next != NULL) {
		*next = name[length] == '/' ? name + length + 1U : NULL;
	}
	return 1;
}

#define SETTINGS_STATIC_HANDLER_DEFINE(name, subtree, h_get, h_set, h_commit, h_export) \
	int settings_host_set_##name(const char *key, size_t len, \
		settings_read_cb read_cb, void *cb_arg) \
	{ \
		return h_set(key, len, read_cb, cb_arg); \
	} \
	int settings_host_commit_##name(void) \
	{ \
		return h_commit(); \
	}

int settings_subsys_init(void);
int settings_load_subtree(const char *subtree);
int settings_save_one(const char *key, const void *value, size_t length);
int settings_load_subtree_direct(const char *subtree,
	settings_load_direct_cb callback, void *param);
int settings_delete(const char *key);

#endif
