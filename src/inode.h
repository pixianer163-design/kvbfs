#ifndef INODE_H
#define INODE_H

#include "kvbfs.h"

/* inode 管理接口 */

/* 分配新 inode 号 */
uint64_t inode_alloc(void);

/* 从缓存或存储获取 inode，增加引用计数 */
struct kvbfs_inode_cache *inode_get(uint64_t ino);

/* 释放 inode 引用 */
void inode_put(struct kvbfs_inode_cache *ic);

/* 创建新 inode */
struct kvbfs_inode_cache *inode_create(uint32_t mode);

/* 删除 inode */
int inode_delete(uint64_t ino);

/* 将 inode 标记为脏 */
void inode_mark_dirty(struct kvbfs_inode_cache *ic);

/* 将脏 inode 写回存储 */
int inode_sync(struct kvbfs_inode_cache *ic);

/* 同步所有脏 inode */
int inode_sync_all(void);

#endif /* INODE_H */
