#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "kvbfs.h"
#include "kv_store.h"

struct kvbfs_ctx *g_ctx = NULL;

/* FUSE 操作表（在 fuse_ops.c 中定义）*/
extern struct fuse_lowlevel_ops kvbfs_ll_ops;

static void usage(const char *progname)
{
    fprintf(stderr, "Usage: %s <mountpoint> <kvstore_path> [options]\n", progname);
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -f        foreground mode\n");
    fprintf(stderr, "  -d        debug mode (implies -f)\n");
    fprintf(stderr, "  -h        show this help\n");
}

int main(int argc, char *argv[])
{
    /* TODO: 实现参数解析和 FUSE 启动 */
    (void)argc;
    (void)argv;

    printf("KVBFS - KV-based File System\n");
    printf("Not yet implemented.\n");
    usage(argv[0]);

    return 0;
}
