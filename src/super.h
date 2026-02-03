#ifndef SUPER_H
#define SUPER_H

#include "kvbfs.h"

/* 加载或创建超级块，返回 0 成功，-1 失败 */
int super_load(struct kvbfs_ctx *ctx);

/* 保存超级块到 KV 存储 */
int super_save(struct kvbfs_ctx *ctx);

/* 创建根目录 inode */
int super_create_root(struct kvbfs_ctx *ctx);

#endif /* SUPER_H */
