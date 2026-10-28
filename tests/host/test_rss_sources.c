/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
/* Exercise the actual cache/registry transaction code and frozen job identity. */
#include <assert.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../src/papermono_rss.c"

static char test_root[128];
static unsigned temporary_serial;
static char article[PAPERMONO_RSS_BODY_MAX + 1U];
static struct papermono_rss_feed first_feed, second_feed;
static struct papermono_rss_source configured[2] = {
    { .id = "gold", .name = "金价", .url = "http://gold.test:8080/feed" },
    { .id = "ai", .name = "AI资讯", .url = "http://ai.test/feed" },
};
static const char gold_xml[] = "<rss><channel><title>Gold</title><item>"
    "<title>Gold update</title><description>gold body</description></item></channel></rss>";
static const char ai_xml[] = "<rss><channel><title>AI</title><item>"
    "<title>AI update</title><description>ai body</description></item></channel></rss>";

static void remove_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) return;
    if (!S_ISDIR(st.st_mode)) { assert(unlink(path) == 0); return; }
    DIR *dir = opendir(path);
    assert(dir != NULL);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[512];
        assert(snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) < (int)sizeof(child));
        remove_tree(child);
    }
    closedir(dir);
    assert(rmdir(path) == 0);
}

static void begin(void)
{
    if (test_root[0]) remove_tree(test_root);
    int ret;
    do {
        snprintf(test_root, sizeof(test_root), "/tmp/papermono-rss-sources-%ld-%u",
                 (long)getpid(), ++temporary_serial);
        ret = mkdir(test_root, 0700);
    } while (ret < 0 && errno == EEXIST);
    assert(ret == 0);
    papermono_rss_test_set_cache_dir(test_root);
    assert(papermono_rss_sources_replace(configured, 2U) == 0);
}

static void reload_registry(void)
{
    papermono_rss_test_set_cache_dir(test_root);
    assert(papermono_rss_source_count() == 2U);
}

static void read_body(size_t source, const char *expected)
{
    memset(article, 0, sizeof(article));
    assert(papermono_rss_body_load_source(source, 0U, article, sizeof(article)) == (int)strlen(expected));
    assert(strcmp(article, expected) == 0);
}

static void seed_both(void)
{
    assert(papermono_rss_test_ingest_source(0U, gold_xml, sizeof(gold_xml) - 1U) == 0);
    assert(papermono_rss_test_ingest_source(1U, ai_xml, sizeof(ai_xml) - 1U) == 0);
}

static void test_independent_cache_and_revision(void)
{
    begin();
    assert(papermono_rss_source_count() == 2U);
    assert(papermono_rss_test_ingest_source(0U, gold_xml, sizeof(gold_xml) - 1U) == 0);
    const uint32_t gold_revision = papermono_rss_revision(0U);
    assert(gold_revision != 0U && papermono_rss_revision(1U) == 0U);
    assert(papermono_rss_test_ingest_source(1U, ai_xml, sizeof(ai_xml) - 1U) == 0);
    assert(papermono_rss_revision(0U) == gold_revision);
    assert(papermono_rss_revision(1U) != 0U);
    read_body(0U, "gold body"); read_body(1U, "ai body");
    papermono_rss_get_source(0U, &first_feed);
    papermono_rss_get_source(1U, &second_feed);
    assert(strcmp(first_feed.title, "Gold") == 0 && strcmp(second_feed.title, "AI") == 0);
    assert(papermono_rss_request_source(0U) == 0);
    assert(papermono_rss_status_source(0U) == 1 && papermono_rss_status_source(1U) == 0);
    assert(papermono_rss_request_source(0U) == 0 && source_states[0].pending);
    const char broken[] = "<html>invalid feed</html>";
    assert(papermono_rss_test_ingest_source(0U, broken, sizeof(broken) - 1U) < 0);
    assert(papermono_rss_revision(0U) == gold_revision);
    assert(papermono_rss_status_source(0U) < 0 && papermono_rss_status_source(1U) == 0);
    read_body(0U, "gold body"); read_body(1U, "ai body");
    reload_registry();
    read_body(0U, "gold body"); read_body(1U, "ai body");
}

static void test_reorder_rename_and_url_isolation(void)
{
    begin(); seed_both();
    char gold_key[17]; memcpy(gold_key, source_states[0].key, sizeof(gold_key));
    const uint32_t revision = papermono_rss_revision(0U);
    const uint32_t generation = papermono_rss_sources_generation();
    struct rss_fetch_job old_job;
    assert(papermono_rss_request_source(0U) == 0);
    source_states[0].pending = false;
    source_states[0].inflight = true;
    rss_job_copy_locked(0U, &old_job);
    struct papermono_rss_source reordered[2] = { configured[1], configured[0] };
    snprintf(reordered[1].name, sizeof(reordered[1].name), "金价快讯");
    assert(papermono_rss_sources_replace(reordered, 2U) == 0);
    assert(papermono_rss_sources_generation() != generation);
    assert(strcmp(source_states[1].key, gold_key) == 0 && papermono_rss_revision(1U) == revision);
    assert(!rss_job_current_locked(&old_job));
    assert(source_states[1].pending && !source_states[1].inflight);
    read_body(0U, "ai body"); read_body(1U, "gold body");
    const uint32_t unchanged = papermono_rss_sources_generation();
    assert(papermono_rss_sources_replace(reordered, 2U) == 0);
    assert(papermono_rss_sources_generation() == unchanged);
    snprintf(reordered[1].url, sizeof(reordered[1].url), "http://gold.test/other");
    assert(papermono_rss_sources_replace(reordered, 2U) == 0);
    papermono_rss_get_source(1U, &first_feed);
    assert(!first_feed.valid && strcmp(source_states[1].key, gold_key) != 0);
    assert(papermono_rss_body_load_source(1U, 0U, article, sizeof(article)) == -ENOENT);
    read_body(0U, "ai body");
    char old_path[RSS_CACHE_PATH_MAX];
    assert(rss_build_path(gold_key, old_path, sizeof(old_path), RSS_BODIES_FILE) == 0);
    assert(rss_file_exists(old_path));
    reordered[1] = configured[0];
    assert(papermono_rss_sources_replace(reordered, 2U) == 0);
    read_body(1U, "gold body");
}

static void test_stale_job_cannot_publish(void)
{
    begin(); seed_both();
    struct rss_fetch_job old_job;
    rss_job_copy_locked(0U, &old_job);
    struct papermono_rss_source updated[2] = { configured[0], configured[1] };
    snprintf(updated[0].url, sizeof(updated[0].url), "http://new.test/feed");
    assert(papermono_rss_sources_replace(updated, 2U) == 0);
    assert(papermono_rss_request_source(0U) == 0);
    assert(rss_cache_store(&old_job, gold_xml, sizeof(gold_xml) - 1U, 1800000000, false) == -ESTALE);
    assert(papermono_rss_status_source(0U) == 1);
    papermono_rss_get_source(0U, &first_feed); assert(!first_feed.valid);
    read_body(1U, "ai body");
    /* Simulate an in-flight body that finished after its registry identity was
     * replaced: the final commit guard must reject it, not just the preflight. */
    uint8_t bounce[RSS_WRITE_BOUNCE];
    assert(rss_write_file(old_job.key, RSS_BODIES_TMP_FILE, "old body", 8U, bounce) == 0);
    assert(rss_cache_commit(&old_job, &second_feed, bounce) == -ESTALE);
    assert(rss_leaf_exists(old_job.key, RSS_BODIES_TMP_FILE) == 0);
    assert(papermono_rss_status_source(0U) == 1);
    assert(papermono_rss_sources_replace(NULL, 0U) == 0);
    assert(rss_cache_store(&old_job, gold_xml, sizeof(gold_xml) - 1U, 1800000000, false) == -ESTALE);
    assert(papermono_rss_source_count() == 0U);
    assert(papermono_rss_request_source(0U) == -ENOENT);
    papermono_rss_test_set_cache_dir(test_root);
    assert(papermono_rss_source_count() == 0U); /* Empty list persists. */
}

static void expect_invalid(const struct papermono_rss_source *sources, size_t count)
{
    const uint32_t generation = papermono_rss_sources_generation();
    assert(papermono_rss_sources_replace(sources, count) == -EINVAL);
    assert(papermono_rss_sources_generation() == generation);
    assert(papermono_rss_source_count() == 2U);
}

static void test_validation_and_failed_write(void)
{
    begin();
    struct papermono_rss_source invalid[2] = { configured[0], configured[1] };
    const char *urls[] = { "https://example.test/feed", "http://", "http://a.test:65536/x",
        "http://a.test:000001/x", "http://user@a.test/x", "http://a.test/x#fragment",
        "http://a.test/x\r\nHost:evil", "http://a.test/a b", "http://[::1]/x" };
    for (size_t i = 0U; i < sizeof(urls) / sizeof(urls[0]); ++i) {
        invalid[0] = configured[0];
        snprintf(invalid[0].url, sizeof(invalid[0].url), "%s", urls[i]);
        expect_invalid(invalid, 2U);
    }
    invalid[0] = configured[0]; snprintf(invalid[0].id, sizeof(invalid[0].id), "../bad");
    expect_invalid(invalid, 2U);
    invalid[0] = configured[0]; invalid[0].name[0] = '\0'; expect_invalid(invalid, 2U);
    const char *names[] = { "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "broken\xe4\xb8", "bad\nname", "   ", "　" };
    for (size_t i = 0U; i < sizeof(names) / sizeof(names[0]); ++i) {
        invalid[0] = configured[0]; snprintf(invalid[0].name, sizeof(invalid[0].name), "%s", names[i]);
        expect_invalid(invalid, 2U);
    }
    invalid[0] = configured[0]; invalid[1] = configured[0]; expect_invalid(invalid, 2U);
    invalid[1] = configured[1]; snprintf(invalid[1].url, sizeof(invalid[1].url), "%s", configured[0].url);
    expect_invalid(invalid, 2U);
    invalid[0] = configured[0]; memset(invalid[0].url, 'a', sizeof(invalid[0].url)); expect_invalid(invalid, 2U);
    expect_invalid(NULL, 1U); expect_invalid(configured, 5U);
    /* Replacing with a changed list must not mutate RAM when the durable write
     * cannot be made. A regular file in the parent path deterministically fails. */
    remove_tree(test_root); assert(mkdir(test_root, 0700) == 0);
    char blocked[256]; snprintf(blocked, sizeof(blocked), "%s/papermono", test_root);
    FILE *file = fopen(blocked, "wb"); assert(file); fclose(file);
    const uint32_t generation = papermono_rss_sources_generation();
    assert(papermono_rss_sources_replace(NULL, 0U) < 0);
    assert(papermono_rss_source_count() == 2U && papermono_rss_sources_generation() == generation);
}

static void copy_leaf(const char *from, const char *to)
{
    char source[RSS_CACHE_PATH_MAX], destination[RSS_CACHE_PATH_MAX];
    assert(rss_build_path(NULL, source, sizeof(source), from) == 0);
    assert(rss_build_path(NULL, destination, sizeof(destination), to) == 0);
    FILE *in = fopen(source, "rb"), *out = fopen(destination, "wb"); assert(in && out);
    int ch; while ((ch = fgetc(in)) != EOF) assert(fputc(ch, out) != EOF);
    fclose(in); assert(fclose(out) == 0);
}

static void corrupt_registry(bool append)
{
    char path[RSS_CACHE_PATH_MAX]; assert(rss_build_path(NULL, path, sizeof(path), RSS_SOURCES_FILE) == 0);
    FILE *file = fopen(path, append ? "ab" : "r+b"); assert(file);
    if (append) assert(fputc('x', file) != EOF);
    else { int ch = fgetc(file); assert(ch != EOF); rewind(file); assert(fputc(ch ^ 1, file) != EOF); }
    fclose(file);
}

static void test_registry_recovery_and_exact_size(void)
{
    begin();
    copy_leaf(RSS_SOURCES_FILE, RSS_SOURCES_BAK_FILE);
    corrupt_registry(false);
    reload_registry();
    struct papermono_rss_source source; assert(papermono_rss_source_get(0U, &source) == 0);
    assert(strcmp(source.id, "gold") == 0 && rss_leaf_exists(NULL, RSS_SOURCES_BAK_FILE) == 0);
    /* A fully valid prefix with trailing garbage must not be accepted. */
    corrupt_registry(true);
    papermono_rss_test_set_cache_dir(test_root);
    assert(papermono_rss_source_count() == 1U);
    assert(papermono_rss_source_get(0U, &source) == 0 && strcmp(source.id, "legacy") == 0);
    assert(papermono_rss_sources_replace(configured, 2U) == 0);
    assert(rss_leaf_rename(NULL, RSS_SOURCES_FILE, RSS_SOURCES_BAK_FILE) == 0);
    reload_registry();
    assert(rss_leaf_exists(NULL, RSS_SOURCES_FILE) == 1);
    /* Identity tags protect a keyed cache even if its directory was reused or
     * manually damaged. Another source's cached body remains unaffected. */
    seed_both();
    char path[RSS_CACHE_PATH_MAX];
    assert(rss_build_path(source_states[0].key, path, sizeof(path), RSS_IDENTITY_FILE) == 0);
    FILE *file = fopen(path, "r+b"); assert(file); assert(fputc('x', file) != EOF); fclose(file);
    papermono_rss_test_reset();
    papermono_rss_get_source(0U, &first_feed); assert(!first_feed.valid);
    read_body(1U, "ai body");
}

int main(void)
{
    test_independent_cache_and_revision();
    test_reorder_rename_and_url_isolation();
    test_stale_job_cannot_publish();
    test_validation_and_failed_write();
    test_registry_recovery_and_exact_size();
    remove_tree(test_root);
    puts("RSS sources: durable registry, source isolation, revisions, stale jobs and validation: PASS");
    return 0;
}
