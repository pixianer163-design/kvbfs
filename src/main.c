#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#include "kvbfs.h"
#include "context.h"
#ifdef CFS_LOCAL_LLM
#include "llm.h"
#endif

struct kvbfs_ctx *g_ctx = NULL;

extern struct fuse_lowlevel_ops kvbfs_ll_ops;

static void usage(const char *progname)
{
    fprintf(stderr, "Usage: %s <mountpoint> <kvstore_path> [FUSE options]\n", progname);
    fprintf(stderr, "\nFUSE options:\n");
    fprintf(stderr, "  -f        foreground\n");
    fprintf(stderr, "  -d        debug (implies -f)\n");
    fprintf(stderr, "  -s        single-threaded\n");
}

int main(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct fuse_session *se;
    struct fuse_cmdline_opts opts;
    int ret = -1;

    if (fuse_parse_cmdline(&args, &opts) != 0) {
        return 1;
    }

    if (opts.show_help) {
        usage(argv[0]);
        fuse_cmdline_help();
        fuse_lowlevel_help();
        ret = 0;
        goto out1;
    }

    if (opts.show_version) {
        printf("KVBFS version 0.1\n");
        printf("FUSE library version %s\n", fuse_pkgversion());
        fuse_lowlevel_version();
        ret = 0;
        goto out1;
    }

    if (!opts.mountpoint) {
        fprintf(stderr, "Error: no mountpoint specified\n");
        usage(argv[0]);
        goto out1;
    }

    /* 需要额外参数: kvstore_path */
    /* 简化处理: 使用环境变量或固定路径 */
    const char *db_path = getenv("KVBFS_DB_PATH");
    if (!db_path) {
        db_path = "/tmp/kvbfs_data";
    }

    printf("KVBFS starting...\n");
    printf("  Mountpoint: %s\n", opts.mountpoint);
    printf("  KV store: %s\n", db_path);

    /* 初始化上下文 */
    g_ctx = ctx_init(db_path);
    if (!g_ctx) {
        fprintf(stderr, "Failed to initialize KVBFS\n");
        goto out1;
    }

#ifdef CFS_LOCAL_LLM
    {
        const char *model_path = getenv("CFS_MODEL_PATH");
        if (model_path) {
            const char *s;
            struct llm_config llm_cfg = {
                .model_path = model_path,
                .n_ctx = 4096,
                .n_gpu_layers = 0,
                .max_tokens = 512,
                .temperature = 0.7f,
            };
            if ((s = getenv("CFS_N_CTX")))        llm_cfg.n_ctx = atoi(s);
            if ((s = getenv("CFS_N_GPU_LAYERS")))  llm_cfg.n_gpu_layers = atoi(s);
            if ((s = getenv("CFS_MAX_TOKENS")))    llm_cfg.max_tokens = atoi(s);
            if ((s = getenv("CFS_TEMPERATURE")))   llm_cfg.temperature = (float)atof(s);

            printf("  LLM model: %s\n", llm_cfg.model_path);
            printf("  LLM n_ctx: %d, gpu_layers: %d, max_tokens: %d\n",
                   llm_cfg.n_ctx, llm_cfg.n_gpu_layers, llm_cfg.max_tokens);

            if (ctx_init_llm(g_ctx, &llm_cfg) != 0) {
                fprintf(stderr, "Warning: LLM init failed, continuing without local inference\n");
            }
        } else {
            printf("  LLM: disabled (CFS_MODEL_PATH not set)\n");
        }
    }
#endif

#ifdef CFS_MEMORY
    {
        const char *embed_path = getenv("CFS_EMBED_MODEL_PATH");
        if (embed_path) {
            const char *s;
            struct mem_config mem_cfg = {
                .embed_model_path = embed_path,
                .n_ctx = 512,
                .n_gpu_layers = 0,
            };
            if ((s = getenv("CFS_EMBED_N_CTX")))        mem_cfg.n_ctx = atoi(s);
            if ((s = getenv("CFS_EMBED_N_GPU_LAYERS")))  mem_cfg.n_gpu_layers = atoi(s);

            printf("  Embed model: %s\n", mem_cfg.embed_model_path);
            printf("  Embed n_ctx: %d, gpu_layers: %d\n",
                   mem_cfg.n_ctx, mem_cfg.n_gpu_layers);

            if (ctx_init_mem(g_ctx, &mem_cfg) != 0) {
                fprintf(stderr, "Warning: Memory subsystem init failed, continuing without embeddings\n");
            }
        } else {
            printf("  Memory: disabled (CFS_EMBED_MODEL_PATH not set)\n");
        }
    }
#endif

    /* 创建 FUSE session */
    se = fuse_session_new(&args, &kvbfs_ll_ops,
                          sizeof(kvbfs_ll_ops), NULL);
    if (!se) {
        fprintf(stderr, "Failed to create FUSE session\n");
        goto out2;
    }

    /* 设置信号处理 */
    if (fuse_set_signal_handlers(se) != 0) {
        goto out3;
    }

    /* 挂载 */
    if (fuse_session_mount(se, opts.mountpoint) != 0) {
        goto out4;
    }

    /* 进入前台或后台 */
    fuse_daemonize(opts.foreground);

    /* 主循环 */
    if (opts.singlethread) {
        ret = fuse_session_loop(se);
    } else {
        struct fuse_loop_config loop_config;
        memset(&loop_config, 0, sizeof(loop_config));
        loop_config.clone_fd = opts.clone_fd;
        loop_config.max_idle_threads = opts.max_idle_threads;
        ret = fuse_session_loop_mt(se, &loop_config);
    }

    /* 卸载 */
    fuse_session_unmount(se);

out4:
    fuse_remove_signal_handlers(se);
out3:
    fuse_session_destroy(se);
out2:
    ctx_destroy(g_ctx);
    g_ctx = NULL;
out1:
    free(opts.mountpoint);
    fuse_opt_free_args(&args);

    return ret ? 1 : 0;
}
