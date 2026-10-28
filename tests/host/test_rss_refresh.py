#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Run the real RSS request/worker control flow with deterministic Zephyr fakes.

No network, SD writes or real sleeps. Parsing/cache transactions are covered by
test_rss.c; this test covers when a fetch happens and how completion is published.
RSS_SOURCE may point at an earlier source revision for a regression demonstration.
"""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get("RSS_SOURCE", ROOT / "src/papermono_rss.c")).read_text()


def function(name):
    match = re.search(
        r"^(?:static )?(?:void|int|int64_t|bool) " + name + r"\([^;]*?\)\n\{",
        SOURCE,
        re.MULTILINE,
    )
    if not match:
        raise AssertionError(f"missing C function: {name}")
    depth = 0
    for index in range(SOURCE.index("{", match.start()), len(SOURCE)):
        if SOURCE[index] == "{":
            depth += 1
        elif SOURCE[index] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[match.start():index + 1] + "\n"
    raise AssertionError(f"unclosed C function: {name}")


def constant(name):
    match = re.search(r"^#define " + name + r" (.+)$", SOURCE, re.MULTILINE)
    if not match:
        raise AssertionError(f"missing C constant: {name}")
    return f"#define {name} {match.group(1)}\n"


PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <papermono/rss.h>
#include <papermono/settings.h>
#define ARG_UNUSED(x) (void)(x)
#define K_FOREVER 0
#define RSS_LOG_INF(...) ((void)0)
#define RSS_LOG_WRN(...) ((void)0)
typedef int atomic_t;
static atomic_t fetch_busy, last_result;
static int request_sem;
static struct papermono_rss_feed snapshot;
static struct papermono_preferences preferences;
static bool index_loaded;
static int64_t wall_clock, uptime_ms, last_fetch_uptime_ms;
static int lock_depth, background_refreshes, content_refreshes;
static int fetches, router_calls, ensure_calls, sem_gives, sleep_calls;
static int fetch_result, unavailable_count, loaded_result;
static int network_ready_at, network_checks;
static bool request_during_fetch, request_during_sleep;
static jmp_buf worker_blocked;

static int atomic_get(const atomic_t *value) { return *value; }
static void atomic_set(atomic_t *value, int next) { *value = next; }
static bool atomic_cas(atomic_t *value, int expected, int next) {
    if (*value != expected) return false;
    *value = next;
    return true;
}
static void k_sem_give(int *sem) { assert(*sem < 1); ++*sem; ++sem_gives; }
static int k_sem_take(int *sem, int timeout) {
    (void)timeout;
    if (!*sem) longjmp(worker_blocked, 1);
    --*sem;
    return 0;
}
static void rss_lock_take(void) { assert(!lock_depth); ++lock_depth; }
static void rss_lock_release(void) { assert(lock_depth == 1); --lock_depth; }
static int rss_ensure_index(void) {
    assert(lock_depth == 1);
    ++ensure_calls;
    if (unavailable_count) {
        --unavailable_count;
        return -ENODEV;
    }
    index_loaded = true;
    return loaded_result;
}
const struct papermono_preferences *papermono_settings_get(void) {
    assert(!lock_depth);
    return &preferences;
}
static bool rss_network_ready(void) {
    return ++network_checks >= network_ready_at;
}
static void ensure_default_router(void) { ++router_calls; }
static void k_msleep(int duration) {
    assert(!lock_depth && fetch_busy == 1);
    ++sleep_calls;
    uptime_ms += duration;
    if (request_during_sleep) {
        assert(papermono_rss_request() == 0);
        assert(request_sem == 0);
    }
}
static int rss_fetch_once(const char *url) {
    assert(!lock_depth && fetch_busy == 1);
    assert(strcmp(url, preferences.rss_url) == 0);
    ++fetches;
    if (request_during_fetch) {
        assert(papermono_rss_request() == 0);
        assert(request_sem == 0);
    }
    if (!fetch_result) {
        snprintf(snapshot.title, sizeof(snapshot.title), "new feed %d", fetches);
        snapshot.valid = true;
        snapshot.updated_at = wall_clock;
    }
    return fetch_result;
}
static void papermono_ui_request_background_refresh(void) {
    assert(!lock_depth);
    ++background_refreshes;
}
static void papermono_ui_request_content_refresh(void) {
    assert(!lock_depth && fetch_busy == 0 && last_result == 0);
    ++content_refreshes;
}
'''

# Only needed to compile pre-fix sources. The cache clock decision itself is
# extracted from that revision, so a cached-page regression fails behaviorally.
LEGACY_CLOCK = r'''
static time_t test_time(time_t *out) {
    if (out) *out = (time_t)wall_clock;
    return (time_t)wall_clock;
}
#define time test_time
static int64_t k_uptime_get(void) { return uptime_ms; }
'''

MAIN = r'''
static void clean(void) {
    memset(&snapshot, 0, sizeof(snapshot));
    memset(&preferences, 0, sizeof(preferences));
    snprintf(preferences.rss_url, sizeof(preferences.rss_url), "http://example.test/rss");
    snapshot.valid = true;
    snprintf(snapshot.title, sizeof(snapshot.title), "cached feed");
    snapshot.updated_at = 1793000000;
    index_loaded = true;
    wall_clock = snapshot.updated_at + 1;
    uptime_ms = last_fetch_uptime_ms = 1000;
    fetch_busy = last_result = request_sem = lock_depth = 0;
    background_refreshes = content_refreshes = fetches = router_calls = 0;
    ensure_calls = sem_gives = sleep_calls = fetch_result = unavailable_count = 0;
    loaded_result = 0;
    network_ready_at = 1;
    network_checks = 0;
    request_during_fetch = request_during_sleep = false;
}
static void run_until_blocked(void) {
    if (!setjmp(worker_blocked)) rss_worker(NULL, NULL, NULL);
    assert(lock_depth == 0 && request_sem == 0);
}
static void open_page(void) {
    assert(papermono_rss_request() == 0);
    assert(request_sem == 1); /* Even a one-second-old valid cache must fetch. */
    assert(fetch_busy == 1 && papermono_rss_status() == 1);
}
static void assert_failure(int error) {
    assert(papermono_rss_status() == error && !fetch_busy);
    assert(background_refreshes == 1 && content_refreshes == 0);
    assert(snapshot.valid && strcmp(snapshot.title, "cached feed") == 0);
}
int main(void) {
    /* Creating the worker/reading a cache does not poll in the background. */
    clean();
    run_until_blocked();
    assert(!fetches && !ensure_calls && !background_refreshes && !content_refreshes);

    /* Every open revalidates, regardless of cache age or RTC validity. */
    clean();
    open_page();
    assert(papermono_rss_request() == 0 && request_sem == 1 && sem_gives == 1);
    request_during_fetch = true;
    run_until_blocked();
    assert(fetches == 1 && router_calls == 1 && !fetch_busy);
    assert(papermono_rss_status() == 0 && content_refreshes == 1 && !background_refreshes);
    assert(strcmp(snapshot.title, "new feed 1") == 0);
    open_page();
    run_until_blocked();
    assert(fetches == 2 && content_refreshes == 2);

    clean();
    wall_clock = 0; /* An uncalibrated clock must not make SD cache permanent. */
    open_page();
    run_until_blocked();
    assert(fetches == 1 && content_refreshes == 1);

    clean();
    snapshot.updated_at = 0; /* Timestamp-free cache from this boot. */
    open_page();
    run_until_blocked();
    assert(fetches == 1 && content_refreshes == 1);

    clean();
    index_loaded = false;
    loaded_result = 1; /* SD cache becomes readable before the network result. */
    open_page();
    run_until_blocked();
    assert(fetches == 1 && background_refreshes == 1 && content_refreshes == 1);

    /* Retryable fetch failures retain cached content and publish a terminal
     * error. Repeated opens during either fetch or backoff do not queue work. */
    clean();
    fetch_result = -ETIMEDOUT;
    request_during_fetch = request_during_sleep = true;
    open_page();
    run_until_blocked();
    assert(fetches == RSS_FETCH_ATTEMPTS && sleep_calls == RSS_FETCH_ATTEMPTS - 1);
    assert(sem_gives == 1);
    assert_failure(-ETIMEDOUT);
    fetch_result = 0;
    open_page();
    assert(papermono_rss_status() == 1); /* Clear the preceding error immediately. */
    run_until_blocked();
    assert(content_refreshes == 1 && !fetch_busy);

    clean();
    fetch_result = -EPROTONOSUPPORT;
    open_page();
    run_until_blocked();
    assert(fetches == 1 && !sleep_calls);
    assert_failure(-EPROTONOSUPPORT);

    /* Failures before HTTP must also dismiss "syncing" and permit reopening. */
    clean();
    network_ready_at = 1000;
    request_during_sleep = true;
    open_page();
    run_until_blocked();
    assert(!fetches && uptime_ms == 1000 + RSS_IPV4_WAIT_MS);
    assert_failure(-ENETDOWN);
    network_ready_at = 1;
    open_page();
    run_until_blocked();
    assert(fetches == 1 && content_refreshes == 1);

    clean();
    preferences.rss_url[0] = '\0';
    open_page();
    run_until_blocked();
    assert(!fetches && !network_checks);
    assert_failure(-EINVAL);
    snprintf(preferences.rss_url, sizeof(preferences.rss_url), "http://example.test/rss");
    open_page();
    run_until_blocked();
    assert(fetches == 1 && content_refreshes == 1);

    /* SD startup wait remains one in-flight request. Exhaustion is terminal,
     * and the next open receives the full retry budget rather than inheriting
     * the exhausted counter. */
    clean();
    unavailable_count = 2;
    request_during_sleep = true;
    open_page();
    run_until_blocked();
    assert(ensure_calls == 3 && sleep_calls == 2 && fetches == 1);
    assert(content_refreshes == 1 && !fetch_busy);

    clean();
    unavailable_count = 16;
    request_during_sleep = true;
    open_page();
    run_until_blocked();
    assert(ensure_calls == 16 && sleep_calls == 15 && !fetches);
    assert_failure(-ENODEV);
    unavailable_count = 1;
    open_page();
    run_until_blocked();
    assert(ensure_calls == 18 && sleep_calls == 16 && fetches == 1);
    assert(content_refreshes == 1 && !fetch_busy);

    puts("RSS refresh: page-entry fetch, single-flight, retries, cached fallback and completion events: PASS");
    return 0;
}
'''


class RssRefreshTests(unittest.TestCase):
    def test_actual_worker_and_request(self):
        defines = "".join(constant(name) for name in (
            "RSS_FETCH_ATTEMPTS", "RSS_FETCH_RETRY_MS", "RSS_IPV4_WAIT_MS",
        ))
        legacy = ""
        if "static bool rss_cache_fresh_locked(" in SOURCE:
            defines += constant("RSS_CACHE_FRESH_SECONDS")
            legacy = LEGACY_CLOCK + function("rss_wall_clock") + function("rss_cache_fresh_locked")
        functions = ["rss_wait_ipv4", "rss_worker", "papermono_rss_status", "papermono_rss_request"]
        if "static void rss_fetch_complete(" in SOURCE:
            functions.insert(0, "rss_fetch_complete")
        code = PREFIX + defines + legacy + "\n".join(function(name) for name in functions) + MAIN
        with tempfile.TemporaryDirectory(prefix="papermono-rss-refresh-") as directory:
            source = Path(directory) / "refresh.c"
            executable = Path(directory) / "refresh-test"
            source.write_text(code)
            subprocess.run(
                shlex.split(os.environ.get("CC", "cc")) + [
                    "-std=c17", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-I", str(ROOT / "include"),
                    str(source), "-o", str(executable),
                ],
                check=True,
            )
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
