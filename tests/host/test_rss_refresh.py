#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run the real per-source RSS request/worker control flow with Zephyr fakes.

Storage transactions are covered by test_rss_sources.c. Here the worker actually
runs until its semaphore blocks: no network, filesystem writes or real sleeps.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/papermono_rss.c").read_text()


def block(pattern):
    match = re.search(pattern, SOURCE, re.MULTILINE)
    if not match:
        raise AssertionError(f"missing C definition: {pattern}")
    depth = 0
    for index in range(SOURCE.index("{", match.start()), len(SOURCE)):
        if SOURCE[index] == "{":
            depth += 1
        elif SOURCE[index] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[match.start():index + 1]
    raise AssertionError(f"unclosed C definition: {pattern}")


def function(name):
    return block(r"^(?:static )?(?:inline )?(?:void|int|int64_t|bool|size_t|uint32_t) "
                 + name + r"\([^;]*?\)\s*\{") + "\n"


def constant(name):
    match = re.search(r"^#define " + name + r" (.+)$", SOURCE, re.MULTILINE)
    if not match:
        raise AssertionError(f"missing C constant: {name}")
    return f"#define {name} {match.group(1)}\n"


INCLUDES = r'''
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <papermono/rss.h>
#define __ZEPHYR__ 1
#define ARG_UNUSED(x) (void)(x)
#define K_FOREVER 0
#define RSS_LOG_INF(...) ((void)0)
#define RSS_LOG_WRN(...) ((void)0)
'''

PREFIX = r'''
static struct rss_source_state source_states[PAPERMONO_RSS_SOURCE_MAX];
static size_t source_count, worker_cursor;
static struct rss_fetch_job worker_job;
static int request_sem;
static int lock_depth, background_refreshes, content_refreshes, sem_gives;
static int fetches[PAPERMONO_RSS_SOURCE_MAX], fetch_result[PAPERMONO_RSS_SOURCE_MAX];
static int unavailable[PAPERMONO_RSS_SOURCE_MAX], loaded_result[PAPERMONO_RSS_SOURCE_MAX];
static int ensure_calls[PAPERMONO_RSS_SOURCE_MAX], sleep_calls, router_calls;
static int order[64], order_count;
static int network_checks, network_ready_at;
static int64_t uptime_ms;
static bool duplicate_during_fetch, duplicate_during_sleep;
static bool queue_other_during_fetch, queue_other_during_backoff;
static bool replace_during_fetch, delete_during_fetch, queued_other;
static jmp_buf worker_blocked;

static void k_sem_give(int *sem) { *sem = 1; ++sem_gives; }
static int k_sem_take(int *sem, int timeout) {
    (void)timeout;
    if (!*sem) longjmp(worker_blocked, 1);
    --*sem;
    return 0;
}
static void rss_lock_take(void) { assert(!lock_depth); ++lock_depth; }
static void rss_lock_release(void) { assert(lock_depth == 1); --lock_depth; }
static void rss_registry_ensure_locked(void) { assert(lock_depth == 1); }
static int rss_ensure_index(struct rss_source_state *state) {
    assert(lock_depth == 1);
    const size_t source = (size_t)(state - source_states);
    assert(source < source_count);
    ++ensure_calls[source];
    if (unavailable[source]) { --unavailable[source]; return -ENODEV; }
    state->index_loaded = true;
    const int result = loaded_result[source];
    loaded_result[source] = 0;
    return result;
}
static bool rss_network_ready(void) { return ++network_checks >= network_ready_at; }
static void ensure_default_router(void) { assert(!lock_depth); ++router_calls; }
static void k_msleep(int duration) {
    assert(!lock_depth && worker_job.slot < source_count);
    assert(source_states[worker_job.slot].inflight);
    ++sleep_calls;
    uptime_ms += duration;
    if (duplicate_during_sleep) {
        assert(papermono_rss_request_source(worker_job.slot) == 0);
        assert(!source_states[worker_job.slot].pending);
    }
    if (queue_other_during_backoff && !queued_other && fetches[0] == 1) {
        assert(papermono_rss_request_source(1U) == 0);
        queued_other = true;
    }
}
static bool rss_job_current_locked(const struct rss_fetch_job *job);
static int rss_fetch_once(const struct rss_fetch_job *job) {
    assert(!lock_depth && job->slot < source_count);
    const size_t source = job->slot;
    assert(source_states[source].inflight);
    assert(strcmp(job->source.url, source_states[source].source.url) == 0);
    ++fetches[source];
    assert(order_count < 64);
    order[order_count++] = (int)source;
    if (duplicate_during_fetch) {
        assert(papermono_rss_request_source(source) == 0);
        assert(!source_states[source].pending);
    }
    if (queue_other_during_fetch && source == 0U && !queued_other) {
        assert(papermono_rss_request_source(1U) == 0);
        queued_other = true;
    }
    if (replace_during_fetch && source == 0U && fetches[0] == 1) {
        ++source_states[0].generation;
        snprintf(source_states[0].source.url, sizeof(source_states[0].source.url), "http://replacement.test/rss");
        source_states[0].inflight = false;
        source_states[0].result = 0;
        assert(papermono_rss_request_source(0U) == 0);
        return 0; /* Completion of this old job must not clear the new request. */
    }
    if (delete_during_fetch && source == 0U) {
        source_count = 0U;
        return 0;
    }
    if (!fetch_result[source] && rss_job_current_locked(job)) {
        snprintf(source_states[source].feed.title,
                 sizeof(source_states[source].feed.title), "new source %zu", source);
        ++source_states[source].revision;
    }
    return fetch_result[source];
}
static void papermono_ui_request_background_refresh(void) {
    assert(!lock_depth); ++background_refreshes;
}
static void papermono_ui_request_content_refresh(void) {
    assert(!lock_depth); ++content_refreshes;
}
'''

MAIN = r'''
static void clean(void) {
    memset(source_states, 0, sizeof(source_states));
    memset(fetches, 0, sizeof(fetches));
    memset(fetch_result, 0, sizeof(fetch_result));
    memset(unavailable, 0, sizeof(unavailable));
    memset(loaded_result, 0, sizeof(loaded_result));
    memset(ensure_calls, 0, sizeof(ensure_calls));
    source_count = 2U;
    for (size_t i = 0U; i < source_count; ++i) {
        struct rss_source_state *state = &source_states[i];
        snprintf(state->source.id, sizeof(state->source.id), "s%zu", i);
        snprintf(state->source.url, sizeof(state->source.url), "http://source%zu.test/rss", i);
        snprintf(state->key, sizeof(state->key), "key%zu", i);
        state->generation = 1U;
        state->index_loaded = true;
        state->feed.valid = true;
        snprintf(state->feed.title, sizeof(state->feed.title), "cached source %zu", i);
        state->feed.updated_at = 0; /* Unknown RTC/cache age cannot suppress work. */
    }
    worker_cursor = 0U;
    request_sem = lock_depth = background_refreshes = content_refreshes = 0;
    sem_gives = sleep_calls = router_calls = order_count = network_checks = 0;
    network_ready_at = 1;
    uptime_ms = 0;
    duplicate_during_fetch = duplicate_during_sleep = false;
    queue_other_during_fetch = queue_other_during_backoff = queued_other = false;
    replace_during_fetch = delete_during_fetch = false;
}
static void run_until_blocked(void) {
    if (!setjmp(worker_blocked)) rss_worker(NULL, NULL, NULL);
    assert(!lock_depth && !request_sem);
}
static void open_source(size_t source) {
    assert(papermono_rss_request_source(source) == 0);
    assert(papermono_rss_status_source(source) == 1);
}
int main(void) {
    /* A worker with no explicit page/pull request never polls any source. */
    clean(); run_until_blocked();
    assert(!fetches[0] && !fetches[1] && !ensure_calls[0] && !ensure_calls[1]);

    /* Legacy source-zero entry, every open, both queued and in-flight coalescing. */
    clean();
    assert(papermono_rss_request() == 0 && papermono_rss_status() == 1);
    assert(request_sem == 1 && sem_gives == 1);
    assert(papermono_rss_request() == 0 && sem_gives == 1);
    duplicate_during_fetch = true;
    run_until_blocked();
    assert(fetches[0] == 1 && !fetches[1] && content_refreshes == 1);
    assert(!source_states[0].inflight && papermono_rss_status() == 0);
    assert(strcmp(source_states[0].feed.title, "new source 0") == 0);
    open_source(0U); run_until_blocked();
    assert(fetches[0] == 2 && content_refreshes == 2);

    /* Two explicit requests share a binary semaphore but both get fetched. */
    clean(); open_source(0U); open_source(1U); run_until_blocked();
    assert(fetches[0] == 1 && fetches[1] == 1 && content_refreshes == 2);
    assert(order_count == 2 && order[0] == 0 && order[1] == 1);
    assert(!source_states[0].pending && !source_states[1].pending);

    /* Round-robin fairness starts at the next source when both are queued. */
    clean(); open_source(0U); run_until_blocked();
    open_source(0U); open_source(1U); run_until_blocked();
    assert(order_count == 3 && order[0] == 0 && order[1] == 1 && order[2] == 0);

    /* A broken source yields to another tab instead of holding it behind
     * 20-second backoff. This is not periodic polling of unrequested sources. */
    clean(); fetch_result[0] = -ETIMEDOUT; queue_other_during_fetch = true;
    open_source(0U); run_until_blocked();
    assert(order_count == 2 && order[0] == 0 && order[1] == 1);
    assert(fetches[0] == 1 && fetches[1] == 1 && uptime_ms == 0);
    assert(papermono_rss_status_source(0U) == -ETIMEDOUT);
    assert(background_refreshes == 1 && content_refreshes == 1);
    assert(strcmp(source_states[0].feed.title, "cached source 0") == 0);

    clean(); fetch_result[0] = -ETIMEDOUT; queue_other_during_backoff = true;
    open_source(0U); run_until_blocked();
    assert(order_count == 2 && order[0] == 0 && order[1] == 1);
    assert(uptime_ms <= 1000 && fetches[0] == 1 && fetches[1] == 1);

    /* Without another tab waiting, preserve the existing bounded retry policy. */
    clean(); fetch_result[0] = -ETIMEDOUT;
    duplicate_during_fetch = duplicate_during_sleep = true;
    open_source(0U); run_until_blocked();
    assert(fetches[0] == RSS_FETCH_ATTEMPTS && !fetches[1]);
    assert(uptime_ms == (RSS_FETCH_ATTEMPTS - 1) * RSS_FETCH_RETRY_MS);
    assert(sem_gives == 1 && papermono_rss_status_source(0U) == -ETIMEDOUT);
    assert(background_refreshes == 1 && !content_refreshes);
    fetch_result[0] = 0; open_source(0U); run_until_blocked();
    assert(papermono_rss_status_source(0U) == 0 && content_refreshes == 1);

    clean(); fetch_result[0] = -EPROTONOSUPPORT;
    open_source(0U); run_until_blocked();
    assert(fetches[0] == 1 && !sleep_calls && background_refreshes == 1);

    /* Failed global prerequisites publish terminal state and allow reopening. */
    clean(); network_ready_at = 1000; duplicate_during_sleep = true;
    open_source(0U); run_until_blocked();
    assert(!fetches[0] && uptime_ms == RSS_IPV4_WAIT_MS);
    assert(papermono_rss_status_source(0U) == -ENETDOWN && background_refreshes == 1);
    network_ready_at = 1; open_source(0U); run_until_blocked();
    assert(fetches[0] == 1 && content_refreshes == 1);

    clean(); unavailable[0] = 16; duplicate_during_sleep = true;
    open_source(0U); run_until_blocked();
    assert(ensure_calls[0] == 16 && sleep_calls == 15 && !fetches[0]);
    assert(papermono_rss_status_source(0U) == -ENODEV && background_refreshes == 1);
    unavailable[0] = 1; open_source(0U); run_until_blocked();
    assert(ensure_calls[0] == 18 && sleep_calls == 16 && fetches[0] == 1);

    clean(); source_states[0].index_loaded = false; loaded_result[0] = 1;
    open_source(0U); run_until_blocked();
    assert(background_refreshes == 1 && content_refreshes == 1);

    /* Replaced/deleted configurations cannot receive an old job's completion. */
    clean(); replace_during_fetch = true;
    open_source(0U); run_until_blocked();
    assert(fetches[0] == 2 && content_refreshes == 1 && !background_refreshes);
    assert(source_states[0].generation == 2U && papermono_rss_status_source(0U) == 0);
    assert(source_states[0].revision == 1U);

    clean(); delete_during_fetch = true;
    open_source(0U); run_until_blocked();
    assert(fetches[0] == 1 && !background_refreshes && !content_refreshes);
    assert(papermono_rss_request_source(0U) == -ENOENT);
    assert(papermono_rss_status_source(0U) == -ENOENT);

    puts("RSS worker: per-source single-flight, fairness, retry yielding and stale completion isolation: PASS");
    return 0;
}
'''


class RssRefreshTests(unittest.TestCase):
    def test_actual_worker_and_requests(self):
        definitions = "".join(block(r"^struct " + name + r"\s*\{") + ";\n" for name in (
            "rss_source_state", "rss_fetch_job",
        ))
        defines = "".join(constant(name) for name in (
            "RSS_FETCH_ATTEMPTS", "RSS_FETCH_RETRY_MS", "RSS_IPV4_WAIT_MS",
        ))
        functions = ("rss_source_equal", "rss_job_current_locked", "rss_job_copy_locked",
                     "rss_job_current", "rss_fetch_complete", "rss_wait_ipv4", "rss_next_job",
                     "rss_retry_delay", "rss_worker", "papermono_rss_request_source",
                     "papermono_rss_status_source", "papermono_rss_request", "papermono_rss_status")
        code = INCLUDES + definitions + defines + PREFIX + "\n".join(function(name) for name in functions) + MAIN
        with tempfile.TemporaryDirectory(prefix="papermono-rss-refresh-") as directory:
            source = Path(directory) / "worker.c"
            executable = Path(directory) / "worker-test"
            source.write_text(code)
            subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
                "-std=c17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "include"),
                str(source), "-o", str(executable),
            ], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
