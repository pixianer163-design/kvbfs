#include "super.h"
#include "kv_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

int super_load(struct kvbfs_ctx *ctx)
{
    char *value = NULL;
    size_t value_len = 0;

    int ret = kv_get(ctx->db, KVBFS_KEY_SUPER, strlen(KVBFS_KEY_SUPER),
                     &value, &value_len);

    if (ret == 0 && value_len == sizeof(struct kvbfs_super)) {
        /* 超级块存在，加载它 */
        memcpy(&ctx->super, value, sizeof(struct kvbfs_super));
        free(value);

        if (ctx->super.magic != KVBFS_MAGIC) {
            fprintf(stderr, "Invalid superblock magic\n");
            return -1;
        }
        return 0;
    }

    if (value) free(value);

    /* 超级块不存在，创建新的 */
    ctx->super.magic = KVBFS_MAGIC;
    ctx->super.version = KVBFS_VERSION;
    ctx->super.next_ino = KVBFS_ROOT_INO + 1;  /* root is ino 1 */

    ret = super_save(ctx);
    if (ret != 0) return ret;

    /* 创建根目录 */
    return super_create_root(ctx);
}

int super_save(struct kvbfs_ctx *ctx)
{
    return kv_put(ctx->db, KVBFS_KEY_SUPER, strlen(KVBFS_KEY_SUPER),
                  (const char *)&ctx->super, sizeof(struct kvbfs_super));
}

int super_create_root(struct kvbfs_ctx *ctx)
{
    struct kvbfs_inode root;
    memset(&root, 0, sizeof(root));

    root.ino = KVBFS_ROOT_INO;
    root.mode = S_IFDIR | 0755;
    root.nlink = 2;  /* . and parent */
    root.size = 0;
    root.blocks = 0;

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    root.atime = now;
    root.mtime = now;
    root.ctime = now;

    char key[64];
    int keylen = kvbfs_key_inode(key, sizeof(key), KVBFS_ROOT_INO);

    return kv_put(ctx->db, key, keylen,
                  (const char *)&root, sizeof(struct kvbfs_inode));
}
