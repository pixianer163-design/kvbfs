#include "inode.h"
#include "kv_store.h"

#include <stdlib.h>
#include <string.h>

/* TODO: 实现 inode 管理函数 */

uint64_t inode_alloc(void)
{
    /* TODO */
    return 0;
}

struct kvbfs_inode_cache *inode_get(uint64_t ino)
{
    (void)ino;
    /* TODO */
    return NULL;
}

void inode_put(struct kvbfs_inode_cache *ic)
{
    (void)ic;
    /* TODO */
}

struct kvbfs_inode_cache *inode_create(uint32_t mode)
{
    (void)mode;
    /* TODO */
    return NULL;
}

int inode_delete(uint64_t ino)
{
    (void)ino;
    /* TODO */
    return -1;
}

void inode_mark_dirty(struct kvbfs_inode_cache *ic)
{
    (void)ic;
    /* TODO */
}

int inode_sync(struct kvbfs_inode_cache *ic)
{
    (void)ic;
    /* TODO */
    return -1;
}

int inode_sync_all(void)
{
    /* TODO */
    return -1;
}
