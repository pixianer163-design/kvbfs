#include "context.h"
#include "kv_store.h"
#include "super.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void ctx_destroy(struct kvbfs_ctx *ctx)
{
    if (!ctx) return;

    /* TODO: 同步所有脏 inode */

    /* 保存超级块 */
    super_save(ctx);

    /* 关闭 KV 存储 */
    if (ctx->db) {
        kv_close(ctx->db);
    }

    /* 销毁锁 */
    pthread_mutex_destroy(&ctx->icache_lock);
    pthread_mutex_destroy(&ctx->alloc_lock);

    /* TODO: 释放 inode 缓存 */

    free(ctx);
}
