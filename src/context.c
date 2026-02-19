#include "context.h"
#include "kv_store.h"
#include "super.h"
#include "inode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef CFS_LOCAL_LLM
/* 查找或创建 /sessions 目录，返回其 inode 号 */
static uint64_t ensure_sessions_dir(struct kvbfs_ctx *ctx)
{
    const char *name = "sessions";

    /* 在根目录下查找 */
    char key[KVBFS_KEY_MAX];
    int keylen = kvbfs_key_dirent(key, sizeof(key), KVBFS_ROOT_INO, name);
    if (keylen < 0) return 0;

    char *val = NULL;
    size_t vlen = 0;
    if (kv_get(ctx->db, key, keylen, &val, &vlen) == 0 && vlen == sizeof(uint64_t)) {
        uint64_t ino;
        memcpy(&ino, val, sizeof(uint64_t));
        free(val);
        printf("CFS: found /sessions (ino=%lu)\n", (unsigned long)ino);
        return ino;
    }
    if (val) free(val);

    /* 不存在，创建 */
    struct kvbfs_inode_cache *ic = inode_create(S_IFDIR | 0755);
    if (!ic) {
        fprintf(stderr, "CFS: failed to create /sessions\n");
        return 0;
    }

    pthread_rwlock_wrlock(&ic->lock);
    ic->inode.nlink = 2;
    pthread_rwlock_unlock(&ic->lock);
    inode_sync(ic);

    uint64_t ino = ic->inode.ino;
    inode_put(ic);

    /* 添加目录项 */
    if (kv_put(ctx->db, key, keylen, (const char *)&ino, sizeof(uint64_t)) != 0) {
        fprintf(stderr, "CFS: failed to add /sessions dirent\n");
        inode_delete(ino);
        return 0;
    }

    /* 增加根目录 nlink */
    struct kvbfs_inode_cache *root = inode_get(KVBFS_ROOT_INO);
    if (root) {
        pthread_rwlock_wrlock(&root->lock);
        root->inode.nlink++;
        pthread_rwlock_unlock(&root->lock);
        inode_sync(root);
        inode_put(root);
    }

    printf("CFS: created /sessions (ino=%lu)\n", (unsigned long)ino);
    return ino;
}
#endif

struct kvbfs_ctx *ctx_init(const char *db_path)
{
    struct kvbfs_ctx *ctx = calloc(1, sizeof(struct kvbfs_ctx));
    if (!ctx) {
        perror("calloc");
        return NULL;
    }

    /* 打开 KV 存储 */
    ctx->db = kv_open(db_path);
    if (!ctx->db) {
        fprintf(stderr, "Failed to open KV store at %s\n", db_path);
        free(ctx);
        return NULL;
    }

    /* 初始化锁 */
    pthread_mutex_init(&ctx->icache_lock, NULL);
    pthread_mutex_init(&ctx->alloc_lock, NULL);

    /* inode 缓存初始化为空 */
    ctx->icache = NULL;

    /* 加载超级块 */
    if (super_load(ctx) != 0) {
        fprintf(stderr, "Failed to load superblock\n");
        kv_close(ctx->db);
        free(ctx);
        return NULL;
    }

    return ctx;
}

#ifdef CFS_LOCAL_LLM
int ctx_init_llm(struct kvbfs_ctx *ctx, const struct llm_config *config)
{
    /* 确保 /sessions 存在 */
    ctx->sessions_ino = ensure_sessions_dir(ctx);
    if (ctx->sessions_ino == 0) {
        fprintf(stderr, "CFS: cannot determine sessions directory\n");
        return -1;
    }

    /* Initialize session_set hash for O(1) is_session_file lookup */
    ctx->session_set = NULL;
    pthread_mutex_init(&ctx->session_lock, NULL);

    /* Populate session_set from /sessions directory entries */
    char prefix[64];
    int prefix_len = kvbfs_key_dirent_prefix(prefix, sizeof(prefix),
                                              ctx->sessions_ino);
    kv_iterator_t *iter = kv_iter_prefix(ctx->db, prefix, prefix_len);
    while (kv_iter_valid(iter)) {
        size_t vlen;
        const char *val = kv_iter_value(iter, &vlen);
        if (vlen == sizeof(uint64_t)) {
            uint64_t child_ino;
            memcpy(&child_ino, val, sizeof(uint64_t));

            struct session_ino_entry *entry = malloc(sizeof(*entry));
            if (entry) {
                entry->ino = child_ino;
                HASH_ADD(hh, ctx->session_set, ino, sizeof(uint64_t), entry);
            }
        }
        kv_iter_next(iter);
    }
    kv_iter_free(iter);

    printf("CFS: loaded %u session inodes into hash set\n",
           HASH_COUNT(ctx->session_set));

    /* 初始化 LLM 子系统 */
    return llm_init(&ctx->llm, config);
}
#endif

#ifdef CFS_MEMORY
int ctx_init_mem(struct kvbfs_ctx *ctx, const struct mem_config *config)
{
    return mem_init(&ctx->mem, config);
}
#endif

void ctx_destroy(struct kvbfs_ctx *ctx)
{
    if (!ctx) return;

#ifdef CFS_MEMORY
    mem_destroy(&ctx->mem);
#endif

#ifdef CFS_LOCAL_LLM
    llm_destroy(&ctx->llm);

    /* Free session_set hash */
    {
        struct session_ino_entry *entry, *tmp;
        HASH_ITER(hh, ctx->session_set, entry, tmp) {
            HASH_DEL(ctx->session_set, entry);
            free(entry);
        }
        pthread_mutex_destroy(&ctx->session_lock);
    }
#endif

    /* 同步所有脏 inode */
    inode_sync_all();

    /* 释放 inode 缓存 */
    inode_cache_clear();

    /* 保存超级块 */
    super_save(ctx);

    /* 关闭 KV 存储 */
    if (ctx->db) {
        kv_close(ctx->db);
    }

    /* 销毁锁 */
    pthread_mutex_destroy(&ctx->icache_lock);
    pthread_mutex_destroy(&ctx->alloc_lock);

    free(ctx);
}
