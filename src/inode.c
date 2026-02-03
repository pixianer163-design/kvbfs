#include "inode.h"
#include "kv_store.h"
#include "super.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

uint64_t inode_alloc(void)
{
    pthread_mutex_lock(&g_ctx->alloc_lock);
    uint64_t ino = g_ctx->super.next_ino++;
    super_save(g_ctx);  /* 持久化 */
    pthread_mutex_unlock(&g_ctx->alloc_lock);
    return ino;
}

int inode_load(uint64_t ino, struct kvbfs_inode *inode)
{
    char key[64];
    int keylen = kvbfs_key_inode(key, sizeof(key), ino);

    char *value = NULL;
    size_t value_len = 0;

    int ret = kv_get(g_ctx->db, key, keylen, &value, &value_len);
    if (ret != 0 || value_len != sizeof(struct kvbfs_inode)) {
        if (value) free(value);
        return -1;
    }

    memcpy(inode, value, sizeof(struct kvbfs_inode));
    free(value);
    return 0;
}

int inode_save(const struct kvbfs_inode *inode)
{
    char key[64];
    int keylen = kvbfs_key_inode(key, sizeof(key), inode->ino);

    return kv_put(g_ctx->db, key, keylen,
                  (const char *)inode, sizeof(struct kvbfs_inode));
}

struct kvbfs_inode_cache *inode_get(uint64_t ino)
{
    struct kvbfs_inode_cache *ic = NULL;

    /* 先查缓存 */
    pthread_mutex_lock(&g_ctx->icache_lock);
    HASH_FIND(hh, g_ctx->icache, &ino, sizeof(uint64_t), ic);
    if (ic) {
        ic->refcount++;
        pthread_mutex_unlock(&g_ctx->icache_lock);
        return ic;
    }
    pthread_mutex_unlock(&g_ctx->icache_lock);

    /* 缓存未命中，从存储加载 */
    struct kvbfs_inode inode;
    if (inode_load(ino, &inode) != 0) {
        return NULL;
    }

    /* 创建缓存项 */
    ic = calloc(1, sizeof(struct kvbfs_inode_cache));
    if (!ic) return NULL;

    ic->inode = inode;
    ic->refcount = 1;
    ic->dirty = false;
    pthread_rwlock_init(&ic->lock, NULL);

    /* 加入缓存 */
    pthread_mutex_lock(&g_ctx->icache_lock);
    /* 双重检查：可能其他线程已加载 */
    struct kvbfs_inode_cache *existing = NULL;
    HASH_FIND(hh, g_ctx->icache, &ino, sizeof(uint64_t), existing);
    if (existing) {
        existing->refcount++;
        pthread_mutex_unlock(&g_ctx->icache_lock);
        pthread_rwlock_destroy(&ic->lock);
        free(ic);
        return existing;
    }
    HASH_ADD(hh, g_ctx->icache, inode.ino, sizeof(uint64_t), ic);
    pthread_mutex_unlock(&g_ctx->icache_lock);

    return ic;
}

void inode_put(struct kvbfs_inode_cache *ic)
{
    if (!ic) return;

    pthread_mutex_lock(&g_ctx->icache_lock);
    if (ic->refcount > 0) {
        ic->refcount--;
    }
    /* 暂不从缓存移除，保留以便复用 */
    pthread_mutex_unlock(&g_ctx->icache_lock);
}

struct kvbfs_inode_cache *inode_create(uint32_t mode)
{
    uint64_t ino = inode_alloc();

    struct kvbfs_inode_cache *ic = calloc(1, sizeof(struct kvbfs_inode_cache));
    if (!ic) return NULL;

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    ic->inode.ino = ino;
    ic->inode.mode = mode;
    ic->inode.nlink = 1;
    ic->inode.size = 0;
    ic->inode.blocks = 0;
    ic->inode.atime = now;
    ic->inode.mtime = now;
    ic->inode.ctime = now;

    ic->refcount = 1;
    ic->dirty = true;
    pthread_rwlock_init(&ic->lock, NULL);

    /* 立即保存到存储 */
    if (inode_save(&ic->inode) != 0) {
        pthread_rwlock_destroy(&ic->lock);
        free(ic);
        return NULL;
    }
    ic->dirty = false;

    /* 加入缓存 */
    pthread_mutex_lock(&g_ctx->icache_lock);
    HASH_ADD(hh, g_ctx->icache, inode.ino, sizeof(uint64_t), ic);
    pthread_mutex_unlock(&g_ctx->icache_lock);

    return ic;
}

int inode_delete(uint64_t ino)
{
    char key[64];
    int keylen = kvbfs_key_inode(key, sizeof(key), ino);

    /* 从缓存移除 */
    pthread_mutex_lock(&g_ctx->icache_lock);
    struct kvbfs_inode_cache *ic = NULL;
    HASH_FIND(hh, g_ctx->icache, &ino, sizeof(uint64_t), ic);
    if (ic) {
        HASH_DEL(g_ctx->icache, ic);
        pthread_rwlock_destroy(&ic->lock);
        free(ic);
    }
    pthread_mutex_unlock(&g_ctx->icache_lock);

    /* 从存储删除 */
    return kv_delete(g_ctx->db, key, keylen);
}

void inode_mark_dirty(struct kvbfs_inode_cache *ic)
{
    if (ic) {
        ic->dirty = true;
    }
}

int inode_sync(struct kvbfs_inode_cache *ic)
{
    if (!ic || !ic->dirty) return 0;

    pthread_rwlock_rdlock(&ic->lock);
    int ret = inode_save(&ic->inode);
    pthread_rwlock_unlock(&ic->lock);

    if (ret == 0) {
        ic->dirty = false;
    }
    return ret;
}

int inode_sync_all(void)
{
    int ret = 0;

    pthread_mutex_lock(&g_ctx->icache_lock);
    struct kvbfs_inode_cache *ic, *tmp;
    HASH_ITER(hh, g_ctx->icache, ic, tmp) {
        if (ic->dirty) {
            if (inode_sync(ic) != 0) {
                ret = -1;
            }
        }
    }
    pthread_mutex_unlock(&g_ctx->icache_lock);

    return ret;
}

void inode_cache_clear(void)
{
    pthread_mutex_lock(&g_ctx->icache_lock);
    struct kvbfs_inode_cache *ic, *tmp;
    HASH_ITER(hh, g_ctx->icache, ic, tmp) {
        HASH_DEL(g_ctx->icache, ic);
        pthread_rwlock_destroy(&ic->lock);
        free(ic);
    }
    pthread_mutex_unlock(&g_ctx->icache_lock);
}
