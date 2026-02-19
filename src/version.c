#include "version.h"
#include "kv_store.h"
#include "inode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read the version counter for an inode; returns 0 if not set */
uint64_t version_get_current(uint64_t ino)
{
    char key[64];
    int keylen = kvbfs_key_version_counter(key, sizeof(key), ino);

    char *val = NULL;
    size_t vlen = 0;
    if (kv_get(g_ctx->db, key, keylen, &val, &vlen) != 0 ||
        vlen != sizeof(uint64_t)) {
        free(val);
        return 0;
    }

    uint64_t ver;
    memcpy(&ver, val, sizeof(uint64_t));
    free(val);
    return ver;
}

/* Write the version counter */
static int version_set_counter(uint64_t ino, uint64_t ver)
{
    char key[64];
    int keylen = kvbfs_key_version_counter(key, sizeof(key), ino);
    return kv_put(g_ctx->db, key, keylen, (const char *)&ver, sizeof(uint64_t));
}

int version_get_meta(uint64_t ino, uint64_t ver, struct kvbfs_version_meta *meta)
{
    char key[64];
    int keylen = kvbfs_key_version_meta(key, sizeof(key), ino, ver);

    char *val = NULL;
    size_t vlen = 0;
    if (kv_get(g_ctx->db, key, keylen, &val, &vlen) != 0 ||
        vlen != sizeof(struct kvbfs_version_meta)) {
        free(val);
        return -1;
    }

    memcpy(meta, val, sizeof(*meta));
    free(val);
    return 0;
}

int version_read_block(uint64_t ino, uint64_t ver, uint64_t block,
                       char **data, size_t *len)
{
    char key[96];
    int keylen = kvbfs_key_version_block(key, sizeof(key), ino, ver, block);
    return kv_get(g_ctx->db, key, keylen, data, len);
}

/* Delete all blocks for a specific version */
static void version_delete_one(uint64_t ino, uint64_t ver)
{
    /* Delete metadata */
    char key[64];
    int keylen = kvbfs_key_version_meta(key, sizeof(key), ino, ver);
    kv_delete(g_ctx->db, key, keylen);

    /* Delete all blocks for this version */
    char prefix[64];
    int prefix_len = kvbfs_key_version_block_prefix(prefix, sizeof(prefix), ino, ver);

    kv_iterator_t *iter = kv_iter_prefix(g_ctx->db, prefix, prefix_len);
    while (kv_iter_valid(iter)) {
        size_t klen;
        const char *k = kv_iter_key(iter, &klen);
        kv_delete(g_ctx->db, k, klen);
        kv_iter_next(iter);
    }
    kv_iter_free(iter);
}

int version_snapshot(uint64_t ino)
{
    /* Get current file metadata */
    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) return -1;

    pthread_rwlock_rdlock(&ic->lock);
    uint64_t file_size = ic->inode.size;
    uint64_t file_blocks = ic->inode.blocks;
    struct timespec file_mtime = ic->inode.mtime;
    pthread_rwlock_unlock(&ic->lock);

    inode_put(ic);

    /* Skip empty files */
    if (file_size == 0) return 0;

    /* Get current version counter */
    uint64_t ver = version_get_current(ino);

    /* Copy current blocks to versioned keys */
    for (uint64_t i = 0; i < file_blocks; i++) {
        char src_key[64];
        int src_keylen = kvbfs_key_block(src_key, sizeof(src_key), ino, i);

        char *block_data = NULL;
        size_t block_len = 0;
        if (kv_get(g_ctx->db, src_key, src_keylen, &block_data, &block_len) != 0)
            continue;

        char dst_key[96];
        int dst_keylen = kvbfs_key_version_block(dst_key, sizeof(dst_key), ino, ver, i);
        kv_put(g_ctx->db, dst_key, dst_keylen, block_data, block_len);
        free(block_data);
    }

    /* Store version metadata */
    struct kvbfs_version_meta meta = {
        .size = file_size,
        .blocks = file_blocks,
        .mtime = file_mtime,
    };
    char meta_key[64];
    int meta_keylen = kvbfs_key_version_meta(meta_key, sizeof(meta_key), ino, ver);
    kv_put(g_ctx->db, meta_key, meta_keylen,
           (const char *)&meta, sizeof(meta));

    /* Increment version counter */
    version_set_counter(ino, ver + 1);

    /* Prune oldest version if we exceeded the limit */
    if (ver + 1 > KVBFS_MAX_VERSIONS) {
        uint64_t oldest = ver + 1 - KVBFS_MAX_VERSIONS;
        version_delete_one(ino, oldest - 1);
    }

    return 0;
}

void version_delete_all(uint64_t ino)
{
    uint64_t ver = version_get_current(ino);
    if (ver == 0) return;

    /* Delete version counter */
    char key[64];
    int keylen = kvbfs_key_version_counter(key, sizeof(key), ino);
    kv_delete(g_ctx->db, key, keylen);

    /* Delete all version metadata and blocks */
    uint64_t start = 0;
    if (ver > KVBFS_MAX_VERSIONS) {
        start = ver - KVBFS_MAX_VERSIONS;
    }
    for (uint64_t v = start; v < ver; v++) {
        version_delete_one(ino, v);
    }
}
