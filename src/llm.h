#ifndef LLM_H
#define LLM_H

#ifdef CFS_LOCAL_LLM

#include <pthread.h>
#include <stdint.h>
#include <fuse_lowlevel.h>

/* LLM 配置 */
struct llm_config {
    const char *model_path;     /* GGUF 模型文件路径 */
    int n_ctx;                  /* 上下文窗口大小 */
    int n_gpu_layers;           /* GPU offload 层数 */
    int max_tokens;             /* 最大生成 token 数 */
    float temperature;          /* 采样温度 */
};

#define LLM_MAX_POLL_WAITERS 16

/* 生成状态（per-inode，uthash 管理） */
struct llm_gen_state {
    uint64_t ino;
    int generating;                                  /* 0=idle, 1=generating */
    struct fuse_pollhandle *waiters[LLM_MAX_POLL_WAITERS];
    int n_waiters;
    UT_hash_handle hh;
};

/* 推理任务 */
struct llm_task {
    uint64_t ino;               /* session 文件 inode */
    struct llm_task *next;
};

/* LLM 子系统上下文 */
struct llm_ctx {
    struct llama_model *model;
    struct llama_context *ctx;

    /* 推理线程 */
    pthread_t thread;
    int running;

    /* 任务队列 */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    struct llm_task *head;
    struct llm_task *tail;
    int shutdown;

    /* 配置 */
    struct llm_config config;

    /* 生成状态 */
    struct llm_gen_state *gen_states;   /* uthash 表 */
    pthread_mutex_t gen_lock;           /* 保护 gen_states */
};

/* 初始化 LLM 子系统：加载模型，启动推理线程 */
int llm_init(struct llm_ctx *llm, const struct llm_config *config);

/* 关闭 LLM 子系统：停止线程，卸载模型 */
void llm_destroy(struct llm_ctx *llm);

/* 提交推理任务（非阻塞） */
int llm_submit(struct llm_ctx *llm, uint64_t ino);

/* 生成状态管理 */
void llm_gen_start(struct llm_ctx *llm, uint64_t ino);
void llm_gen_finish(struct llm_ctx *llm, uint64_t ino);
int  llm_gen_is_active(struct llm_ctx *llm, uint64_t ino);
void llm_gen_add_waiter(struct llm_ctx *llm, uint64_t ino, struct fuse_pollhandle *ph);
void llm_gen_destroy(struct llm_ctx *llm);

#endif /* CFS_LOCAL_LLM */
#endif /* LLM_H */
