#ifndef INODE_H
#define INODE_H

#include "kvbfs.h"

/* inode 管理接口 */

/* 分配新 inode 号 */
uint64_t inode_alloc(void);

/* 从 KV 存储加载 inode（不使用缓存） */
int inode_load(uint64_t ino, struct kvbfs_inode *inode);

/* 保存 inode 到 KV 存储 */
int inode_save(const struct kvbfs_inode *inode);

/* 从缓存或存储获取 inode，增加引用计数 */
struct kvbfs_inode_cache *inode_get(uint64_t ino);

/* 释放 inode 引用 */
void inode_put(struct kvbfs_inode_cache *ic);

/* 创建新 inode */
struct kvbfs_inode_cache *inode_create(uint32_t mode);

/* 删除 inode（从存储中删除） */
int inode_delete(uint64_t ino);

/* 将 inode 标记为脏 */
void inode_mark_dirty(struct kvbfs_inode_cache *ic);

/* 将脏 inode 写回存储 */
int inode_sync(struct kvbfs_inode_cache *ic);

/* 同步所有脏 inode */
int inode_sync_all(void);

/* 释放所有缓存的 inode */
void inode_cache_clear(void);

#endif /* INODE_H */
