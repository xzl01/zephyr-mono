/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stddef.h>

static unsigned calls;
static void *last_node;

void __real_ap_try_sa_query_process(void *node)
{
	++calls;
	last_node = node;
}

void __wrap_ap_try_sa_query_process(void *node);

int main(void)
{
	int station;

	__wrap_ap_try_sa_query_process(NULL);
	assert(calls == 0);
	__wrap_ap_try_sa_query_process(&station);
	assert(calls == 1 && last_node == &station);
	__wrap_ap_try_sa_query_process(NULL);
	assert(calls == 1 && last_node == &station);
	return 0;
}
