#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <sys/stat.h>

#include "../src/kvbfs.h"
#include "../src/inode.h"
#include "../src/kv_store.h"
#include "../src/super.h"

/* 测试程序中定义全局上下文（主程序中在 main.c 定义） */
struct kvbfs_ctx *g_ctx = NULL;

#define TEST_DB_PATH "/tmp/test_inode_db"

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(name) do { \
    printf("  %-50s", #name); \
    tests_run++; \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
} while (0)

/* Helper: init a fresh test context */
static void setup(void)
{
    /* Clean up previous DB */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_DB_PATH);
    system(cmd);

    g_ctx = calloc(1, sizeof(struct kvbfs_ctx));
    assert(g_ctx);

    g_ctx->db = kv_open(TEST_DB_PATH);
    assert(g_ctx->db);

    pthread_mutex_init(&g_ctx->icache_lock, NULL);
    pthread_mutex_init(&g_ctx->alloc_lock, NULL);
    g_ctx->icache = NULL;

    /* Init superblock */
    assert(super_load(g_ctx) == 0);
}

static void teardown(void)
{
    inode_cache_clear();

    if (g_ctx->db) kv_close(g_ctx->db);
    pthread_mutex_destroy(&g_ctx->icache_lock);
    pthread_mutex_destroy(&g_ctx->alloc_lock);
    free(g_ctx);
    g_ctx = NULL;
}

/* Test 1: create/get/put lifecycle */
static void test_create_get_put(void)
{
    setup();

    struct kvbfs_inode_cache *ic = inode_create(S_IFREG | 0644);
    assert(ic != NULL);
    uint64_t ino = ic->inode.ino;
    assert(ino > 0);
    assert(ic->inode.mode == (S_IFREG | 0644));
    assert(ic->inode.nlink == 1);
    assert(ic->refcount == 1);
    inode_put(ic);

    /* Should be retrievable from cache */
    struct kvbfs_inode_cache *ic2 = inode_get(ino);
    assert(ic2 != NULL);
    assert(ic2->inode.ino == ino);
    assert(ic2->inode.mode == (S_IFREG | 0644));
    inode_put(ic2);

    /* Should also be loadable directly from storage */
    struct kvbfs_inode raw;
    assert(inode_load(ino, &raw) == 0);
    assert(raw.ino == ino);

    teardown();
}

/* Test 2: refcount tracking */
static void test_refcount_tracking(void)
{
    setup();

    struct kvbfs_inode_cache *ic = inode_create(S_IFREG | 0644);
    assert(ic != NULL);
    uint64_t ino = ic->inode.ino;
    assert(ic->refcount == 1);

    /* Multiple gets increase refcount */
    struct kvbfs_inode_cache *ic2 = inode_get(ino);
    assert(ic2 == ic);
    assert(ic->refcount == 2);

    struct kvbfs_inode_cache *ic3 = inode_get(ino);
    assert(ic3 == ic);
    assert(ic->refcount == 3);

    /* Puts decrease refcount */
    inode_put(ic3);
    assert(ic->refcount == 2);

    inode_put(ic2);
    assert(ic->refcount == 1);

    inode_put(ic);
    assert(ic->refcount == 0);

    teardown();
}

/* Test 3: delete with no active references */
static void test_delete_no_refs(void)
{
    setup();

    struct kvbfs_inode_cache *ic = inode_create(S_IFREG | 0644);
    assert(ic != NULL);
    uint64_t ino = ic->inode.ino;
    inode_put(ic);  /* refcount -> 0 */

    /* Delete should succeed */
    assert(inode_delete(ino) == 0);

    /* get should return NULL (inode gone from both cache and storage) */
    struct kvbfs_inode_cache *ic2 = inode_get(ino);
    assert(ic2 == NULL);

    /* Load from storage should fail too */
    struct kvbfs_inode raw;
    assert(inode_load(ino, &raw) != 0);

    teardown();
}

/* Test 4: delete with active references (P0-1 validation) */
static void test_delete_with_active_refs(void)
{
    setup();

    struct kvbfs_inode_cache *ic = inode_create(S_IFREG | 0644);
    assert(ic != NULL);
    uint64_t ino = ic->inode.ino;
    assert(ic->refcount == 1);

    /* Get a second reference */
    struct kvbfs_inode_cache *ic2 = inode_get(ino);
    assert(ic2 == ic);
    assert(ic->refcount == 2);

    /* Delete while refs are held — should mark deleted, not free */
    assert(inode_delete(ino) == 0);

    /* Further inode_get should return NULL (deleted flag) */
    struct kvbfs_inode_cache *ic3 = inode_get(ino);
    assert(ic3 == NULL);

    /* Existing references are still valid (no UAF) — can access fields */
    assert(ic->inode.ino == ino);
    assert(ic->deleted == true);

    /* Put references back; the last put should free the cache entry */
    inode_put(ic);   /* refcount 2 -> 1 */
    inode_put(ic2);  /* refcount 1 -> 0, should be freed */

    /* Confirm it's gone from cache: get returns NULL */
    struct kvbfs_inode_cache *ic4 = inode_get(ino);
    assert(ic4 == NULL);

    teardown();
}

/* Test 5: concurrent get/put — 8 threads, 1000 iterations each */
struct thread_args {
    uint64_t ino;
    int iterations;
};

static void *thread_get_put(void *arg)
{
    struct thread_args *ta = (struct thread_args *)arg;

    for (int i = 0; i < ta->iterations; i++) {
        struct kvbfs_inode_cache *ic = inode_get(ta->ino);
        if (!ic) continue;

        /* Briefly hold the reference */
        pthread_rwlock_rdlock(&ic->lock);
        assert(ic->inode.ino == ta->ino);
        pthread_rwlock_unlock(&ic->lock);

        inode_put(ic);
    }

    return NULL;
}

static void test_concurrent_get_put(void)
{
    setup();

    struct kvbfs_inode_cache *ic = inode_create(S_IFREG | 0644);
    assert(ic != NULL);
    uint64_t ino = ic->inode.ino;
    inode_put(ic);  /* refcount -> 0, stays in cache */

    #define NUM_THREADS 8
    #define ITERATIONS 1000

    pthread_t threads[NUM_THREADS];
    struct thread_args args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].ino = ino;
        args[i].iterations = ITERATIONS;
        assert(pthread_create(&threads[i], NULL, thread_get_put, &args[i]) == 0);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* After all threads done, refcount should be 0 */
    struct kvbfs_inode_cache *check = inode_get(ino);
    assert(check != NULL);
    /* refcount should be 1 (from our get just now) */
    assert(check->refcount == 1);
    inode_put(check);

    teardown();

    #undef NUM_THREADS
    #undef ITERATIONS
}

/* Test 6: concurrent delete — one deleter, others hold references */
struct delete_thread_args {
    uint64_t ino;
    int iterations;
    int do_delete;  /* only one thread does the delete */
};

static void *thread_delete_or_hold(void *arg)
{
    struct delete_thread_args *ta = (struct delete_thread_args *)arg;

    if (ta->do_delete) {
        /* Wait a bit for other threads to acquire refs */
        for (volatile int i = 0; i < 10000; i++) {}
        inode_delete(ta->ino);
    } else {
        for (int i = 0; i < ta->iterations; i++) {
            struct kvbfs_inode_cache *ic = inode_get(ta->ino);
            if (!ic) break;  /* deleted, stop */

            /* Hold briefly */
            pthread_rwlock_rdlock(&ic->lock);
            (void)ic->inode.ino;
            pthread_rwlock_unlock(&ic->lock);

            inode_put(ic);
        }
    }

    return NULL;
}

static void test_concurrent_delete(void)
{
    setup();

    struct kvbfs_inode_cache *ic = inode_create(S_IFREG | 0644);
    assert(ic != NULL);
    uint64_t ino = ic->inode.ino;
    inode_put(ic);

    #define NUM_THREADS2 8

    pthread_t threads[NUM_THREADS2];
    struct delete_thread_args args[NUM_THREADS2];

    /* Thread 0 deletes, others do get/put */
    for (int i = 0; i < NUM_THREADS2; i++) {
        args[i].ino = ino;
        args[i].iterations = 500;
        args[i].do_delete = (i == 0);
        assert(pthread_create(&threads[i], NULL, thread_delete_or_hold, &args[i]) == 0);
    }

    for (int i = 0; i < NUM_THREADS2; i++) {
        pthread_join(threads[i], NULL);
    }

    /* After all threads complete, inode should be gone */
    struct kvbfs_inode_cache *check = inode_get(ino);
    assert(check == NULL);

    teardown();

    #undef NUM_THREADS2
}

int main(void)
{
    printf("Testing inode management...\n");

    RUN_TEST(test_create_get_put);
    RUN_TEST(test_refcount_tracking);
    RUN_TEST(test_delete_no_refs);
    RUN_TEST(test_delete_with_active_refs);
    RUN_TEST(test_concurrent_get_put);
    RUN_TEST(test_concurrent_delete);

    printf("\n%d/%d tests passed!\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
