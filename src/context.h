#ifndef CONTEXT_H
#define CONTEXT_H

#include "kvbfs.h"

/* 初始化文件系统上下文 */
struct kvbfs_ctx *ctx_init(const char *db_path);

/* 销毁上下文，释放资源 */
void ctx_destroy(struct kvbfs_ctx *ctx);

#endif /* CONTEXT_H */
