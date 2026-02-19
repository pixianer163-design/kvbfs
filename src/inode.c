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
    pthread_mutex_unlock(&g_ctx->alloc_lock);
    super_save(g_ctx);  /* I/O outside lock; crash wastes at most one ino */
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
        if (ic->deleted) {
            pthread_mutex_unlock(&g_ctx->icache_lock);
            return NULL;  /* deleted inode, treat as absent */
        }
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
    /* 双重检查：可能其他线程已加载或已删除 */
    struct kvbfs_inode_cache *existing = NULL;
    HASH_FIND(hh, g_ctx->icache, &ino, sizeof(uint64_t), existing);
    if (existing) {
        if (existing->deleted) {
            pthread_mutex_unlock(&g_ctx->icache_lock);
            pthread_rwlock_destroy(&ic->lock);
            free(ic);
            return NULL;
        }
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
    if (ic->refcount == 0 && ic->deleted) {
        HASH_DEL(g_ctx->icache, ic);
        pthread_mutex_unlock(&g_ctx->icache_lock);
        pthread_rwlock_destroy(&ic->lock);
        free(ic);
        return;
    }
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

    /* Mark deleted; free immediately only if refcount == 0 */
    pthread_mutex_lock(&g_ctx->icache_lock);
    struct kvbfs_inode_cache *ic = NULL;
    HASH_FIND(hh, g_ctx->icache, &ino, sizeof(uint64_t), ic);
    if (ic) {
        ic->deleted = true;
        if (ic->refcount == 0) {
            HASH_DEL(g_ctx->icache, ic);
            pthread_mutex_unlock(&g_ctx->icache_lock);
            pthread_rwlock_destroy(&ic->lock);
            free(ic);
            return kv_delete(g_ctx->db, key, keylen);
        }
        /* refcount > 0: keep in hash marked deleted; inode_put will clean up */
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

    /* 收集脏 inode 并增加引用计数，避免持锁时调用 inode_sync */
    size_t count = 0;
    size_t capacity = 64;
    struct kvbfs_inode_cache **dirty_list = malloc(capacity * sizeof(*dirty_list));
    if (!dirty_list) return -1;

    pthread_mutex_lock(&g_ctx->icache_lock);
    struct kvbfs_inode_cache *ic, *tmp;
    HASH_ITER(hh, g_ctx->icache, ic, tmp) {
        if (ic->dirty && !ic->deleted) {
            ic->refcount++;
            if (count >= capacity) {
                capacity *= 2;
                struct kvbfs_inode_cache **new_list = realloc(dirty_list, capacity * sizeof(*dirty_list));
                if (!new_list) {
                    /* 回退已增加的引用计数 */
                    for (size_t i = 0; i < count; i++) {
                        dirty_list[i]->refcount--;
                    }
                    pthread_mutex_unlock(&g_ctx->icache_lock);
                    free(dirty_list);
                    return -1;
                }
                dirty_list = new_list;
            }
            dirty_list[count++] = ic;
        }
    }
    pthread_mutex_unlock(&g_ctx->icache_lock);

    /* 在无锁状态下逐个同步 */
    for (size_t i = 0; i < count; i++) {
        if (inode_sync(dirty_list[i]) != 0) {
            ret = -1;
        }
        inode_put(dirty_list[i]);
    }

    free(dirty_list);
    return ret;
}

void inode_cache_clear(void)
{
    pthread_mutex_lock(&g_ctx->icache_lock);
    struct kvbfs_inode_cache *ic, *tmp;
    HASH_ITER(hh, g_ctx->icache, ic, tmp) {
        if (ic->refcount > 0) {
            fprintf(stderr, "warning: inode %lu still has refcount %lu at cache clear\n",
                    (unsigned long)ic->inode.ino, (unsigned long)ic->refcount);
        }
        HASH_DEL(g_ctx->icache, ic);
        pthread_rwlock_destroy(&ic->lock);
        free(ic);
    }
    pthread_mutex_unlock(&g_ctx->icache_lock);
}
