#ifndef CONTEXT_H
#define CONTEXT_H

#include "kvbfs.h"

/* 初始化文件系统上下文 */
struct kvbfs_ctx *ctx_init(const char *db_path);

#ifdef CFS_LOCAL_LLM
/* 初始化 LLM 子系统（在 ctx_init 之后调用） */
int ctx_init_llm(struct kvbfs_ctx *ctx, const struct llm_config *config);
#endif

#ifdef CFS_MEMORY
/* 初始化 Memory/embedding 子系统（在 ctx_init 之后调用） */
int ctx_init_mem(struct kvbfs_ctx *ctx, const struct mem_config *config);
#endif

/* 销毁上下文，释放资源 */
void ctx_destroy(struct kvbfs_ctx *ctx);

#endif /* CONTEXT_H */
