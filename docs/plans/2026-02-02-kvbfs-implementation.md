# KVBFS Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement a working FUSE-based file system backed by RocksDB KV storage.

**Architecture:** Two-layer design - FUSE operations layer directly manages inode cache and calls KV storage layer. RocksDB backend already implemented.

**Tech Stack:** C, libfuse3, RocksDB, uthash, pthread

**Testing:** Since this is a Linux-only project developed on Windows, tests will be run in WSL2. Unit tests use simple assert-based testing.

---

## Phase 1: Superblock and Initialization

### Task 1.1: Add KV Key Helper Functions

**Files:**
- Modify: `src/kvbfs.h`

**Step 1: Add key formatting helpers to kvbfs.h**

Add after the `extern struct kvbfs_ctx *g_ctx;` line:

```c
/* KV key 常量 */
#define KVBFS_KEY_SUPER     "sb"
#define KVBFS_KEY_NEXT_INO  "next_ino"

/* KV key 格式化辅助函数 */
static inline int kvbfs_key_inode(char *buf, size_t buflen, uint64_t ino)
{
    return snprintf(buf, buflen, "i:%lu", (unsigned long)ino);
}

static inline int kvbfs_key_dirent(char *buf, size_t buflen, uint64_t parent, const char *name)
{
    return snprintf(buf, buflen, "d:%lu:%s", (unsigned long)parent, name);
}

static inline int kvbfs_key_block(char *buf, size_t buflen, uint64_t ino, uint64_t block)
{
    return snprintf(buf, buflen, "b:%lu:%lu", (unsigned long)ino, (unsigned long)block);
}

static inline int kvbfs_key_dirent_prefix(char *buf, size_t buflen, uint64_t parent)
{
    return snprintf(buf, buflen, "d:%lu:", (unsigned long)parent);
}
```

**Step 2: Commit**

```bash
git add src/kvbfs.h
git commit -m "feat: add KV key formatting helpers"
```

---

### Task 1.2: Implement Superblock Functions

**Files:**
- Create: `src/super.c`
- Create: `src/super.h`
- Modify: `CMakeLists.txt`

**Step 1: Create super.h**

```c
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
```

**Step 2: Create super.c**

```c
#include "super.h"
#include "kv_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

int super_load(struct kvbfs_ctx *ctx)
{
    char *value = NULL;
    size_t value_len = 0;

    int ret = kv_get(ctx->db, KVBFS_KEY_SUPER, strlen(KVBFS_KEY_SUPER),
                     &value, &value_len);

    if (ret == 0 && value_len == sizeof(struct kvbfs_super)) {
        /* 超级块存在，加载它 */
        memcpy(&ctx->super, value, sizeof(struct kvbfs_super));
        free(value);

        if (ctx->super.magic != KVBFS_MAGIC) {
            fprintf(stderr, "Invalid superblock magic\n");
            return -1;
        }
        return 0;
    }

    if (value) free(value);

    /* 超级块不存在，创建新的 */
    ctx->super.magic = KVBFS_MAGIC;
    ctx->super.version = KVBFS_VERSION;
    ctx->super.next_ino = KVBFS_ROOT_INO + 1;  /* root is ino 1 */

    ret = super_save(ctx);
    if (ret != 0) return ret;

    /* 创建根目录 */
    return super_create_root(ctx);
}

int super_save(struct kvbfs_ctx *ctx)
{
    return kv_put(ctx->db, KVBFS_KEY_SUPER, strlen(KVBFS_KEY_SUPER),
                  (const char *)&ctx->super, sizeof(struct kvbfs_super));
}

int super_create_root(struct kvbfs_ctx *ctx)
{
    struct kvbfs_inode root;
    memset(&root, 0, sizeof(root));

    root.ino = KVBFS_ROOT_INO;
    root.mode = S_IFDIR | 0755;
    root.nlink = 2;  /* . and parent */
    root.size = 0;
    root.blocks = 0;

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    root.atime = now;
    root.mtime = now;
    root.ctime = now;

    char key[64];
    int keylen = kvbfs_key_inode(key, sizeof(key), KVBFS_ROOT_INO);

    return kv_put(ctx->db, key, keylen,
                  (const char *)&root, sizeof(struct kvbfs_inode));
}
```

**Step 3: Update CMakeLists.txt**

Add `src/super.c` to KVBFS_SOURCES list:

```cmake
set(KVBFS_SOURCES
    src/main.c
    src/fuse_ops.c
    src/super.c
    src/kv_store.c
    src/kv_rocksdb.c
    src/inode.c
    src/utils.c
)
```

**Step 4: Commit**

```bash
git add src/super.h src/super.c CMakeLists.txt
git commit -m "feat: implement superblock load/save and root directory creation"
```

---

### Task 1.3: Implement Context Initialization

**Files:**
- Modify: `src/kvbfs.h`
- Create: `src/context.c`
- Create: `src/context.h`
- Modify: `CMakeLists.txt`

**Step 1: Create context.h**

```c
#ifndef CONTEXT_H
#define CONTEXT_H

#include "kvbfs.h"

/* 初始化文件系统上下文 */
struct kvbfs_ctx *ctx_init(const char *db_path);

/* 销毁上下文，释放资源 */
void ctx_destroy(struct kvbfs_ctx *ctx);

#endif /* CONTEXT_H */
```

**Step 2: Create context.c**

```c
#include "context.h"
#include "kv_store.h"
#include "super.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct kvbfs_ctx *ctx_init(const char *db_path)
{
    struct kvbfs_ctx *ctx = calloc(1, sizeof(struct kvbfs_ctx));
    if (!ctx) {
        perror("calloc");
        return NULL;
    }

    /* 打开 KV 存储 */
    ctx->db = kv_open(db_path);
    if (!ctx->db) {
        fprintf(stderr, "Failed to open KV store at %s\n", db_path);
        free(ctx);
        return NULL;
    }

    /* 初始化锁 */
    pthread_mutex_init(&ctx->icache_lock, NULL);
    pthread_mutex_init(&ctx->alloc_lock, NULL);

    /* inode 缓存初始化为空 */
    ctx->icache = NULL;

    /* 加载超级块 */
    if (super_load(ctx) != 0) {
        fprintf(stderr, "Failed to load superblock\n");
        kv_close(ctx->db);
        free(ctx);
        return NULL;
    }

    return ctx;
}

void ctx_destroy(struct kvbfs_ctx *ctx)
{
    if (!ctx) return;

    /* TODO: 同步所有脏 inode */

    /* 保存超级块 */
    super_save(ctx);

    /* 关闭 KV 存储 */
    if (ctx->db) {
        kv_close(ctx->db);
    }

    /* 销毁锁 */
    pthread_mutex_destroy(&ctx->icache_lock);
    pthread_mutex_destroy(&ctx->alloc_lock);

    /* TODO: 释放 inode 缓存 */

    free(ctx);
}
```

**Step 3: Update CMakeLists.txt**

Add `src/context.c` to KVBFS_SOURCES:

```cmake
set(KVBFS_SOURCES
    src/main.c
    src/fuse_ops.c
    src/super.c
    src/context.c
    src/kv_store.c
    src/kv_rocksdb.c
    src/inode.c
    src/utils.c
)
```

**Step 4: Commit**

```bash
git add src/context.h src/context.c CMakeLists.txt
git commit -m "feat: implement context initialization and destruction"
```

---

### Task 1.4: Implement main.c FUSE Startup

**Files:**
- Modify: `src/main.c`

**Step 1: Rewrite main.c with FUSE session setup**

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#include "kvbfs.h"
#include "context.h"

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
```

**Step 2: Commit**

```bash
git add src/main.c
git commit -m "feat: implement FUSE session setup in main.c"
```

---

## Phase 2: inode Management

### Task 2.1: Implement inode Allocation

**Files:**
- Modify: `src/inode.c`
- Modify: `src/inode.h`

**Step 1: Update inode.h with additional declarations**

```c
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
```

**Step 2: Implement inode.c**

```c
#include "inode.h"
#include "kv_store.h"
#include "super.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

uint64_t inode_alloc(void)
{
    pthread_mutex_lock(&g_ctx->alloc_lock);
    uint64_t ino = g_ctx->super.next_ino++;
    super_save(g_ctx);  /* 持久化 */
    pthread_mutex_unlock(&g_ctx->alloc_lock);
    return ino;
}

int inode_load(uint64_t ino, struct kvbfs_inode *inode)
{
    char key[64];
    int keylen = kvbfs_key_inode(key, sizeof(key), ino);

    char *value = NULL;
    size_t value_len = 0;

    int ret = kv_get(g_ctx->db, key, keylen, &value, &value_len);
    if (ret != 0 || value_len != sizeof(struct kvbfs_inode)) {
        if (value) free(value);
        return -1;
    }

    memcpy(inode, value, sizeof(struct kvbfs_inode));
    free(value);
    return 0;
}

int inode_save(const struct kvbfs_inode *inode)
{
    char key[64];
    int keylen = kvbfs_key_inode(key, sizeof(key), inode->ino);

    return kv_put(g_ctx->db, key, keylen,
                  (const char *)inode, sizeof(struct kvbfs_inode));
}

struct kvbfs_inode_cache *inode_get(uint64_t ino)
{
    struct kvbfs_inode_cache *ic = NULL;

    /* 先查缓存 */
    pthread_mutex_lock(&g_ctx->icache_lock);
    HASH_FIND(hh, g_ctx->icache, &ino, sizeof(uint64_t), ic);
    if (ic) {
        ic->refcount++;
        pthread_mutex_unlock(&g_ctx->icache_lock);
        return ic;
    }
    pthread_mutex_unlock(&g_ctx->icache_lock);

    /* 缓存未命中，从存储加载 */
    struct kvbfs_inode inode;
    if (inode_load(ino, &inode) != 0) {
        return NULL;
    }

    /* 创建缓存项 */
    ic = calloc(1, sizeof(struct kvbfs_inode_cache));
    if (!ic) return NULL;

    ic->inode = inode;
    ic->refcount = 1;
    ic->dirty = false;
    pthread_rwlock_init(&ic->lock, NULL);

    /* 加入缓存 */
    pthread_mutex_lock(&g_ctx->icache_lock);
    /* 双重检查：可能其他线程已加载 */
    struct kvbfs_inode_cache *existing = NULL;
    HASH_FIND(hh, g_ctx->icache, &ino, sizeof(uint64_t), existing);
    if (existing) {
        existing->refcount++;
        pthread_mutex_unlock(&g_ctx->icache_lock);
        pthread_rwlock_destroy(&ic->lock);
        free(ic);
        return existing;
    }
    HASH_ADD(hh, g_ctx->icache, inode.ino, sizeof(uint64_t), ic);
    pthread_mutex_unlock(&g_ctx->icache_lock);

    return ic;
}

void inode_put(struct kvbfs_inode_cache *ic)
{
    if (!ic) return;

    pthread_mutex_lock(&g_ctx->icache_lock);
    if (ic->refcount > 0) {
        ic->refcount--;
    }
    /* 暂不从缓存移除，保留以便复用 */
    pthread_mutex_unlock(&g_ctx->icache_lock);
}

struct kvbfs_inode_cache *inode_create(uint32_t mode)
{
    uint64_t ino = inode_alloc();

    struct kvbfs_inode_cache *ic = calloc(1, sizeof(struct kvbfs_inode_cache));
    if (!ic) return NULL;

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    ic->inode.ino = ino;
    ic->inode.mode = mode;
    ic->inode.nlink = 1;
    ic->inode.size = 0;
    ic->inode.blocks = 0;
    ic->inode.atime = now;
    ic->inode.mtime = now;
    ic->inode.ctime = now;

    ic->refcount = 1;
    ic->dirty = true;
    pthread_rwlock_init(&ic->lock, NULL);

    /* 立即保存到存储 */
    if (inode_save(&ic->inode) != 0) {
        pthread_rwlock_destroy(&ic->lock);
        free(ic);
        return NULL;
    }
    ic->dirty = false;

    /* 加入缓存 */
    pthread_mutex_lock(&g_ctx->icache_lock);
    HASH_ADD(hh, g_ctx->icache, inode.ino, sizeof(uint64_t), ic);
    pthread_mutex_unlock(&g_ctx->icache_lock);

    return ic;
}

int inode_delete(uint64_t ino)
{
    char key[64];
    int keylen = kvbfs_key_inode(key, sizeof(key), ino);

    /* 从缓存移除 */
    pthread_mutex_lock(&g_ctx->icache_lock);
    struct kvbfs_inode_cache *ic = NULL;
    HASH_FIND(hh, g_ctx->icache, &ino, sizeof(uint64_t), ic);
    if (ic) {
        HASH_DEL(g_ctx->icache, ic);
        pthread_rwlock_destroy(&ic->lock);
        free(ic);
    }
    pthread_mutex_unlock(&g_ctx->icache_lock);

    /* 从存储删除 */
    return kv_delete(g_ctx->db, key, keylen);
}

void inode_mark_dirty(struct kvbfs_inode_cache *ic)
{
    if (ic) {
        ic->dirty = true;
    }
}

int inode_sync(struct kvbfs_inode_cache *ic)
{
    if (!ic || !ic->dirty) return 0;

    pthread_rwlock_rdlock(&ic->lock);
    int ret = inode_save(&ic->inode);
    pthread_rwlock_unlock(&ic->lock);

    if (ret == 0) {
        ic->dirty = false;
    }
    return ret;
}

int inode_sync_all(void)
{
    int ret = 0;

    pthread_mutex_lock(&g_ctx->icache_lock);
    struct kvbfs_inode_cache *ic, *tmp;
    HASH_ITER(hh, g_ctx->icache, ic, tmp) {
        if (ic->dirty) {
            if (inode_sync(ic) != 0) {
                ret = -1;
            }
        }
    }
    pthread_mutex_unlock(&g_ctx->icache_lock);

    return ret;
}

void inode_cache_clear(void)
{
    pthread_mutex_lock(&g_ctx->icache_lock);
    struct kvbfs_inode_cache *ic, *tmp;
    HASH_ITER(hh, g_ctx->icache, ic, tmp) {
        HASH_DEL(g_ctx->icache, ic);
        pthread_rwlock_destroy(&ic->lock);
        free(ic);
    }
    pthread_mutex_unlock(&g_ctx->icache_lock);
}
```

**Step 3: Commit**

```bash
git add src/inode.h src/inode.c
git commit -m "feat: implement inode allocation, caching, and persistence"
```

---

## Phase 3: Read-Only FUSE Operations

### Task 3.1: Implement getattr

**Files:**
- Modify: `src/fuse_ops.c`

**Step 1: Add helper to convert inode to stat**

Add at the top of fuse_ops.c after includes:

```c
#include "super.h"

static void inode_to_stat(const struct kvbfs_inode *inode, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino = inode->ino;
    st->st_mode = inode->mode;
    st->st_nlink = inode->nlink;
    st->st_size = inode->size;
    st->st_blocks = inode->blocks;
    st->st_blksize = KVBFS_BLOCK_SIZE;
    st->st_atim = inode->atime;
    st->st_mtim = inode->mtime;
    st->st_ctim = inode->ctime;
    st->st_uid = getuid();
    st->st_gid = getgid();
}
```

**Step 2: Implement getattr**

```c
static void kvbfs_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    (void)fi;

    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    struct stat st;
    pthread_rwlock_rdlock(&ic->lock);
    inode_to_stat(&ic->inode, &st);
    pthread_rwlock_unlock(&ic->lock);

    inode_put(ic);

    fuse_reply_attr(req, &st, 1.0);  /* 1 second cache */
}
```

**Step 3: Add required includes at top of fuse_ops.c**

```c
#include <unistd.h>
#include <sys/stat.h>
```

**Step 4: Commit**

```bash
git add src/fuse_ops.c
git commit -m "feat: implement getattr FUSE operation"
```

---

### Task 3.2: Implement lookup

**Files:**
- Modify: `src/fuse_ops.c`

**Step 1: Add directory entry helper functions**

Add after inode_to_stat:

```c
/* 查找目录项，返回子 inode 号，未找到返回 0 */
static uint64_t dirent_lookup(uint64_t parent, const char *name)
{
    char key[256];
    int keylen = kvbfs_key_dirent(key, sizeof(key), parent, name);

    char *value = NULL;
    size_t value_len = 0;

    int ret = kv_get(g_ctx->db, key, keylen, &value, &value_len);
    if (ret != 0 || value_len != sizeof(uint64_t)) {
        if (value) free(value);
        return 0;
    }

    uint64_t child_ino;
    memcpy(&child_ino, value, sizeof(uint64_t));
    free(value);
    return child_ino;
}

/* 添加目录项 */
static int dirent_add(uint64_t parent, const char *name, uint64_t child)
{
    char key[256];
    int keylen = kvbfs_key_dirent(key, sizeof(key), parent, name);

    return kv_put(g_ctx->db, key, keylen,
                  (const char *)&child, sizeof(uint64_t));
}

/* 删除目录项 */
static int dirent_remove(uint64_t parent, const char *name)
{
    char key[256];
    int keylen = kvbfs_key_dirent(key, sizeof(key), parent, name);

    return kv_delete(g_ctx->db, key, keylen);
}
```

**Step 2: Implement lookup**

```c
static void kvbfs_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    uint64_t child_ino = dirent_lookup(parent, name);
    if (child_ino == 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    struct kvbfs_inode_cache *ic = inode_get(child_ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.ino = child_ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;

    pthread_rwlock_rdlock(&ic->lock);
    inode_to_stat(&ic->inode, &e.attr);
    pthread_rwlock_unlock(&ic->lock);

    inode_put(ic);

    fuse_reply_entry(req, &e);
}
```

**Step 3: Commit**

```bash
git add src/fuse_ops.c
git commit -m "feat: implement lookup FUSE operation"
```

---

### Task 3.3: Implement readdir

**Files:**
- Modify: `src/fuse_ops.c`

**Step 1: Implement opendir (add to ops table too)**

```c
static void kvbfs_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_rdlock(&ic->lock);
    int is_dir = S_ISDIR(ic->inode.mode);
    pthread_rwlock_unlock(&ic->lock);

    inode_put(ic);

    if (!is_dir) {
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    fuse_reply_open(req, fi);
}
```

**Step 2: Implement readdir**

```c
static void kvbfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                          off_t off, struct fuse_file_info *fi)
{
    (void)fi;

    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }
    inode_put(ic);

    char *buf = malloc(size);
    if (!buf) {
        fuse_reply_err(req, ENOMEM);
        return;
    }

    size_t buf_used = 0;
    off_t entry_idx = 0;

    /* . and .. entries */
    if (off <= 0) {
        struct stat st = {.st_ino = ino, .st_mode = S_IFDIR};
        size_t entsize = fuse_add_direntry(req, buf + buf_used, size - buf_used,
                                           ".", &st, ++entry_idx);
        if (entsize > size - buf_used) goto done;
        buf_used += entsize;
    }

    if (off <= 1) {
        struct stat st = {.st_ino = ino, .st_mode = S_IFDIR};  /* parent, simplified */
        size_t entsize = fuse_add_direntry(req, buf + buf_used, size - buf_used,
                                           "..", &st, ++entry_idx);
        if (entsize > size - buf_used) goto done;
        buf_used += entsize;
    }

    /* 遍历目录项 */
    char prefix[64];
    int prefix_len = kvbfs_key_dirent_prefix(prefix, sizeof(prefix), ino);

    kv_iterator_t *iter = kv_iter_prefix(g_ctx->db, prefix, prefix_len);
    while (kv_iter_valid(iter)) {
        entry_idx++;

        if (entry_idx <= off) {
            kv_iter_next(iter);
            continue;
        }

        size_t key_len;
        const char *key = kv_iter_key(iter, &key_len);

        /* 提取文件名：跳过 "d:<parent>:" 前缀 */
        const char *name = key + prefix_len;
        size_t name_len = key_len - prefix_len;

        /* 获取子 inode 号 */
        size_t val_len;
        const char *val = kv_iter_value(iter, &val_len);
        uint64_t child_ino;
        memcpy(&child_ino, val, sizeof(uint64_t));

        /* 获取子 inode 属性 */
        struct kvbfs_inode child_inode;
        struct stat st = {0};
        if (inode_load(child_ino, &child_inode) == 0) {
            st.st_ino = child_ino;
            st.st_mode = child_inode.mode;
        }

        /* 需要 null-terminated name */
        char name_buf[256];
        if (name_len >= sizeof(name_buf)) name_len = sizeof(name_buf) - 1;
        memcpy(name_buf, name, name_len);
        name_buf[name_len] = '\0';

        size_t entsize = fuse_add_direntry(req, buf + buf_used, size - buf_used,
                                           name_buf, &st, entry_idx + 1);
        if (entsize > size - buf_used) {
            kv_iter_free(iter);
            goto done;
        }
        buf_used += entsize;

        kv_iter_next(iter);
    }
    kv_iter_free(iter);

done:
    fuse_reply_buf(req, buf, buf_used);
    free(buf);
}
```

**Step 3: Add opendir to ops table**

Add to kvbfs_ll_ops struct:

```c
    .opendir    = kvbfs_opendir,
```

**Step 4: Commit**

```bash
git add src/fuse_ops.c
git commit -m "feat: implement readdir and opendir FUSE operations"
```

---

### Task 3.4: Implement open and read

**Files:**
- Modify: `src/fuse_ops.c`

**Step 1: Implement open**

```c
static void kvbfs_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_rdlock(&ic->lock);
    int is_file = S_ISREG(ic->inode.mode);
    pthread_rwlock_unlock(&ic->lock);

    inode_put(ic);

    if (!is_file) {
        fuse_reply_err(req, EISDIR);
        return;
    }

    fuse_reply_open(req, fi);
}
```

**Step 2: Implement read**

```c
static void kvbfs_read(fuse_req_t req, fuse_ino_t ino, size_t size,
                       off_t off, struct fuse_file_info *fi)
{
    (void)fi;

    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_rdlock(&ic->lock);
    uint64_t file_size = ic->inode.size;
    pthread_rwlock_unlock(&ic->lock);

    inode_put(ic);

    /* 超出文件末尾 */
    if ((uint64_t)off >= file_size) {
        fuse_reply_buf(req, NULL, 0);
        return;
    }

    /* 调整读取大小 */
    if (off + size > file_size) {
        size = file_size - off;
    }

    char *buf = malloc(size);
    if (!buf) {
        fuse_reply_err(req, ENOMEM);
        return;
    }

    size_t bytes_read = 0;
    uint64_t block_idx = off / KVBFS_BLOCK_SIZE;
    size_t block_off = off % KVBFS_BLOCK_SIZE;

    while (bytes_read < size) {
        char key[64];
        int keylen = kvbfs_key_block(key, sizeof(key), ino, block_idx);

        char *block_data = NULL;
        size_t block_len = 0;

        int ret = kv_get(g_ctx->db, key, keylen, &block_data, &block_len);
        if (ret != 0) {
            /* 块不存在，填充零 */
            size_t to_copy = KVBFS_BLOCK_SIZE - block_off;
            if (to_copy > size - bytes_read) to_copy = size - bytes_read;
            memset(buf + bytes_read, 0, to_copy);
            bytes_read += to_copy;
        } else {
            size_t to_copy = block_len - block_off;
            if (to_copy > size - bytes_read) to_copy = size - bytes_read;
            memcpy(buf + bytes_read, block_data + block_off, to_copy);
            bytes_read += to_copy;
            free(block_data);
        }

        block_idx++;
        block_off = 0;  /* 后续块从头开始 */
    }

    fuse_reply_buf(req, buf, bytes_read);
    free(buf);
}
```

**Step 3: Commit**

```bash
git add src/fuse_ops.c
git commit -m "feat: implement open and read FUSE operations"
```

---

## Phase 4: Write Operations - Directories

### Task 4.1: Implement mkdir

**Files:**
- Modify: `src/fuse_ops.c`

**Step 1: Implement mkdir**

```c
static void kvbfs_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode)
{
    /* 检查父目录存在且是目录 */
    struct kvbfs_inode_cache *pic = inode_get(parent);
    if (!pic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_rdlock(&pic->lock);
    int is_dir = S_ISDIR(pic->inode.mode);
    pthread_rwlock_unlock(&pic->lock);

    if (!is_dir) {
        inode_put(pic);
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    /* 检查是否已存在 */
    if (dirent_lookup(parent, name) != 0) {
        inode_put(pic);
        fuse_reply_err(req, EEXIST);
        return;
    }

    /* 创建新目录 inode */
    struct kvbfs_inode_cache *ic = inode_create(S_IFDIR | (mode & 0777));
    if (!ic) {
        inode_put(pic);
        fuse_reply_err(req, EIO);
        return;
    }

    /* 设置 nlink = 2 (. 和 父目录的引用) */
    pthread_rwlock_wrlock(&ic->lock);
    ic->inode.nlink = 2;
    pthread_rwlock_unlock(&ic->lock);
    inode_sync(ic);

    /* 添加目录项 */
    if (dirent_add(parent, name, ic->inode.ino) != 0) {
        inode_delete(ic->inode.ino);
        inode_put(ic);
        inode_put(pic);
        fuse_reply_err(req, EIO);
        return;
    }

    /* 增加父目录 nlink */
    pthread_rwlock_wrlock(&pic->lock);
    pic->inode.nlink++;
    pthread_rwlock_unlock(&pic->lock);
    inode_sync(pic);
    inode_put(pic);

    /* 返回新目录信息 */
    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.ino = ic->inode.ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;

    pthread_rwlock_rdlock(&ic->lock);
    inode_to_stat(&ic->inode, &e.attr);
    pthread_rwlock_unlock(&ic->lock);

    inode_put(ic);

    fuse_reply_entry(req, &e);
}
```

**Step 2: Commit**

```bash
git add src/fuse_ops.c
git commit -m "feat: implement mkdir FUSE operation"
```

---

### Task 4.2: Implement rmdir

**Files:**
- Modify: `src/fuse_ops.c`

**Step 1: Add helper to check if directory is empty**

```c
static int dirent_is_empty(uint64_t ino)
{
    char prefix[64];
    int prefix_len = kvbfs_key_dirent_prefix(prefix, sizeof(prefix), ino);

    kv_iterator_t *iter = kv_iter_prefix(g_ctx->db, prefix, prefix_len);
    int empty = !kv_iter_valid(iter);
    kv_iter_free(iter);

    return empty;
}
```

**Step 2: Implement rmdir**

```c
static void kvbfs_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    /* 查找目标目录 */
    uint64_t child_ino = dirent_lookup(parent, name);
    if (child_ino == 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    struct kvbfs_inode_cache *ic = inode_get(child_ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_rdlock(&ic->lock);
    int is_dir = S_ISDIR(ic->inode.mode);
    pthread_rwlock_unlock(&ic->lock);

    if (!is_dir) {
        inode_put(ic);
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    /* 检查目录是否为空 */
    if (!dirent_is_empty(child_ino)) {
        inode_put(ic);
        fuse_reply_err(req, ENOTEMPTY);
        return;
    }

    /* 删除目录项 */
    if (dirent_remove(parent, name) != 0) {
        inode_put(ic);
        fuse_reply_err(req, EIO);
        return;
    }

    /* 减少父目录 nlink */
    struct kvbfs_inode_cache *pic = inode_get(parent);
    if (pic) {
        pthread_rwlock_wrlock(&pic->lock);
        if (pic->inode.nlink > 0) pic->inode.nlink--;
        pthread_rwlock_unlock(&pic->lock);
        inode_sync(pic);
        inode_put(pic);
    }

    /* 删除 inode */
    inode_put(ic);
    inode_delete(child_ino);

    fuse_reply_err(req, 0);
}
```

**Step 3: Commit**

```bash
git add src/fuse_ops.c
git commit -m "feat: implement rmdir FUSE operation"
```

---

## Phase 5: Write Operations - Files

### Task 5.1: Implement create

**Files:**
- Modify: `src/fuse_ops.c`

**Step 1: Implement create**

```c
static void kvbfs_create(fuse_req_t req, fuse_ino_t parent, const char *name,
                         mode_t mode, struct fuse_file_info *fi)
{
    /* 检查父目录 */
    struct kvbfs_inode_cache *pic = inode_get(parent);
    if (!pic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_rdlock(&pic->lock);
    int is_dir = S_ISDIR(pic->inode.mode);
    pthread_rwlock_unlock(&pic->lock);
    inode_put(pic);

    if (!is_dir) {
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    /* 检查是否已存在 */
    if (dirent_lookup(parent, name) != 0) {
        fuse_reply_err(req, EEXIST);
        return;
    }

    /* 创建新文件 inode */
    struct kvbfs_inode_cache *ic = inode_create(S_IFREG | (mode & 0777));
    if (!ic) {
        fuse_reply_err(req, EIO);
        return;
    }

    /* 添加目录项 */
    if (dirent_add(parent, name, ic->inode.ino) != 0) {
        inode_delete(ic->inode.ino);
        inode_put(ic);
        fuse_reply_err(req, EIO);
        return;
    }

    /* 返回新文件信息 */
    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.ino = ic->inode.ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;

    pthread_rwlock_rdlock(&ic->lock);
    inode_to_stat(&ic->inode, &e.attr);
    pthread_rwlock_unlock(&ic->lock);

    inode_put(ic);

    fuse_reply_create(req, &e, fi);
}
```

**Step 2: Commit**

```bash
git add src/fuse_ops.c
git commit -m "feat: implement create FUSE operation"
```

---

### Task 5.2: Implement write

**Files:**
- Modify: `src/fuse_ops.c`

**Step 1: Implement write**

```c
static void kvbfs_write(fuse_req_t req, fuse_ino_t ino, const char *buf,
                        size_t size, off_t off, struct fuse_file_info *fi)
{
    (void)fi;

    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    size_t bytes_written = 0;
    uint64_t block_idx = off / KVBFS_BLOCK_SIZE;
    size_t block_off = off % KVBFS_BLOCK_SIZE;

    while (bytes_written < size) {
        char key[64];
        int keylen = kvbfs_key_block(key, sizeof(key), ino, block_idx);

        /* 读取现有块（如果存在）或创建新块 */
        char block[KVBFS_BLOCK_SIZE];
        memset(block, 0, KVBFS_BLOCK_SIZE);

        char *existing = NULL;
        size_t existing_len = 0;
        if (kv_get(g_ctx->db, key, keylen, &existing, &existing_len) == 0) {
            size_t copy_len = existing_len < KVBFS_BLOCK_SIZE ? existing_len : KVBFS_BLOCK_SIZE;
            memcpy(block, existing, copy_len);
            free(existing);
        }

        /* 写入数据到块 */
        size_t to_write = KVBFS_BLOCK_SIZE - block_off;
        if (to_write > size - bytes_written) to_write = size - bytes_written;

        memcpy(block + block_off, buf + bytes_written, to_write);

        /* 保存块 */
        size_t block_size = block_off + to_write;
        if (block_size < KVBFS_BLOCK_SIZE) {
            /* 可能块后面还有数据 */
            block_size = KVBFS_BLOCK_SIZE;
        }
        if (kv_put(g_ctx->db, key, keylen, block, block_size) != 0) {
            inode_put(ic);
            fuse_reply_err(req, EIO);
            return;
        }

        bytes_written += to_write;
        block_idx++;
        block_off = 0;
    }

    /* 更新文件大小 */
    pthread_rwlock_wrlock(&ic->lock);
    if ((uint64_t)(off + size) > ic->inode.size) {
        ic->inode.size = off + size;
    }
    ic->inode.blocks = (ic->inode.size + KVBFS_BLOCK_SIZE - 1) / KVBFS_BLOCK_SIZE;

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    ic->inode.mtime = now;
    ic->inode.ctime = now;
    pthread_rwlock_unlock(&ic->lock);

    inode_sync(ic);
    inode_put(ic);

    fuse_reply_write(req, bytes_written);
}
```

**Step 2: Commit**

```bash
git add src/fuse_ops.c
git commit -m "feat: implement write FUSE operation"
```

---

### Task 5.3: Implement unlink

**Files:**
- Modify: `src/fuse_ops.c`

**Step 1: Add helper to delete file blocks**

```c
static void delete_file_blocks(uint64_t ino, uint64_t blocks)
{
    for (uint64_t i = 0; i < blocks; i++) {
        char key[64];
        int keylen = kvbfs_key_block(key, sizeof(key), ino, i);
        kv_delete(g_ctx->db, key, keylen);
    }
}
```

**Step 2: Implement unlink**

```c
static void kvbfs_unlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    /* 查找文件 */
    uint64_t child_ino = dirent_lookup(parent, name);
    if (child_ino == 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    struct kvbfs_inode_cache *ic = inode_get(child_ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_rdlock(&ic->lock);
    int is_dir = S_ISDIR(ic->inode.mode);
    uint64_t blocks = ic->inode.blocks;
    pthread_rwlock_unlock(&ic->lock);

    if (is_dir) {
        inode_put(ic);
        fuse_reply_err(req, EISDIR);
        return;
    }

    /* 删除目录项 */
    if (dirent_remove(parent, name) != 0) {
        inode_put(ic);
        fuse_reply_err(req, EIO);
        return;
    }

    /* 减少 nlink，如果为 0 则删除 */
    pthread_rwlock_wrlock(&ic->lock);
    if (ic->inode.nlink > 0) ic->inode.nlink--;
    int should_delete = (ic->inode.nlink == 0);
    pthread_rwlock_unlock(&ic->lock);

    inode_put(ic);

    if (should_delete) {
        delete_file_blocks(child_ino, blocks);
        inode_delete(child_ino);
    }

    fuse_reply_err(req, 0);
}
```

**Step 3: Commit**

```bash
git add src/fuse_ops.c
git commit -m "feat: implement unlink FUSE operation"
```

---

### Task 5.4: Implement setattr

**Files:**
- Modify: `src/fuse_ops.c`

**Step 1: Implement setattr**

```c
static void kvbfs_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
                          int to_set, struct fuse_file_info *fi)
{
    (void)fi;

    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_wrlock(&ic->lock);

    if (to_set & FUSE_SET_ATTR_MODE) {
        ic->inode.mode = (ic->inode.mode & S_IFMT) | (attr->st_mode & 0777);
    }

    if (to_set & FUSE_SET_ATTR_SIZE) {
        uint64_t old_size = ic->inode.size;
        uint64_t new_size = attr->st_size;

        if (new_size < old_size) {
            /* 截断：删除多余的块 */
            uint64_t old_blocks = (old_size + KVBFS_BLOCK_SIZE - 1) / KVBFS_BLOCK_SIZE;
            uint64_t new_blocks = (new_size + KVBFS_BLOCK_SIZE - 1) / KVBFS_BLOCK_SIZE;

            for (uint64_t i = new_blocks; i < old_blocks; i++) {
                char key[64];
                int keylen = kvbfs_key_block(key, sizeof(key), ino, i);
                kv_delete(g_ctx->db, key, keylen);
            }
        }

        ic->inode.size = new_size;
        ic->inode.blocks = (new_size + KVBFS_BLOCK_SIZE - 1) / KVBFS_BLOCK_SIZE;
    }

    if (to_set & FUSE_SET_ATTR_ATIME) {
        ic->inode.atime = attr->st_atim;
    }

    if (to_set & FUSE_SET_ATTR_MTIME) {
        ic->inode.mtime = attr->st_mtim;
    }

    if (to_set & (FUSE_SET_ATTR_ATIME_NOW | FUSE_SET_ATTR_MTIME_NOW)) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (to_set & FUSE_SET_ATTR_ATIME_NOW) ic->inode.atime = now;
        if (to_set & FUSE_SET_ATTR_MTIME_NOW) ic->inode.mtime = now;
    }

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    ic->inode.ctime = now;

    struct stat st;
    inode_to_stat(&ic->inode, &st);

    pthread_rwlock_unlock(&ic->lock);

    inode_sync(ic);
    inode_put(ic);

    fuse_reply_attr(req, &st, 1.0);
}
```

**Step 2: Commit**

```bash
git add src/fuse_ops.c
git commit -m "feat: implement setattr FUSE operation"
```

---

### Task 5.5: Implement release

**Files:**
- Modify: `src/fuse_ops.c`

**Step 1: Implement release**

```c
static void kvbfs_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    (void)ino;
    (void)fi;
    /* 目前不需要特殊处理 */
    fuse_reply_err(req, 0);
}
```

**Step 2: Commit**

```bash
git add src/fuse_ops.c
git commit -m "feat: implement release FUSE operation"
```

---

## Phase 6: Rename and Finalization

### Task 6.1: Implement rename

**Files:**
- Modify: `src/fuse_ops.c`

**Step 1: Implement rename**

```c
static void kvbfs_rename(fuse_req_t req, fuse_ino_t parent, const char *name,
                         fuse_ino_t newparent, const char *newname, unsigned int flags)
{
    (void)flags;  /* 暂不支持 RENAME_EXCHANGE 等 */

    /* 查找源文件 */
    uint64_t src_ino = dirent_lookup(parent, name);
    if (src_ino == 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    /* 检查目标是否已存在 */
    uint64_t dst_ino = dirent_lookup(newparent, newname);
    if (dst_ino != 0) {
        /* 目标存在，需要先删除 */
        struct kvbfs_inode_cache *dst_ic = inode_get(dst_ino);
        if (dst_ic) {
            pthread_rwlock_rdlock(&dst_ic->lock);
            int is_dir = S_ISDIR(dst_ic->inode.mode);
            uint64_t blocks = dst_ic->inode.blocks;
            pthread_rwlock_unlock(&dst_ic->lock);
            inode_put(dst_ic);

            if (is_dir) {
                if (!dirent_is_empty(dst_ino)) {
                    fuse_reply_err(req, ENOTEMPTY);
                    return;
                }
            }

            dirent_remove(newparent, newname);
            if (!is_dir) {
                delete_file_blocks(dst_ino, blocks);
            }
            inode_delete(dst_ino);
        }
    }

    /* 获取源 inode 信息 */
    struct kvbfs_inode_cache *src_ic = inode_get(src_ino);
    int src_is_dir = 0;
    if (src_ic) {
        pthread_rwlock_rdlock(&src_ic->lock);
        src_is_dir = S_ISDIR(src_ic->inode.mode);
        pthread_rwlock_unlock(&src_ic->lock);
        inode_put(src_ic);
    }

    /* 删除旧目录项 */
    if (dirent_remove(parent, name) != 0) {
        fuse_reply_err(req, EIO);
        return;
    }

    /* 添加新目录项 */
    if (dirent_add(newparent, newname, src_ino) != 0) {
        /* 尝试恢复 */
        dirent_add(parent, name, src_ino);
        fuse_reply_err(req, EIO);
        return;
    }

    /* 如果是目录且跨目录移动，更新父目录 nlink */
    if (src_is_dir && parent != newparent) {
        struct kvbfs_inode_cache *old_pic = inode_get(parent);
        if (old_pic) {
            pthread_rwlock_wrlock(&old_pic->lock);
            if (old_pic->inode.nlink > 0) old_pic->inode.nlink--;
            pthread_rwlock_unlock(&old_pic->lock);
            inode_sync(old_pic);
            inode_put(old_pic);
        }

        struct kvbfs_inode_cache *new_pic = inode_get(newparent);
        if (new_pic) {
            pthread_rwlock_wrlock(&new_pic->lock);
            new_pic->inode.nlink++;
            pthread_rwlock_unlock(&new_pic->lock);
            inode_sync(new_pic);
            inode_put(new_pic);
        }
    }

    fuse_reply_err(req, 0);
}
```

**Step 2: Commit**

```bash
git add src/fuse_ops.c
git commit -m "feat: implement rename FUSE operation"
```

---

### Task 6.2: Update init and destroy

**Files:**
- Modify: `src/fuse_ops.c`

**Step 1: Update init**

```c
static void kvbfs_init(void *userdata, struct fuse_conn_info *conn)
{
    (void)userdata;
    (void)conn;

    /* 上下文已在 main.c 中初始化 */
    printf("KVBFS initialized\n");
}
```

**Step 2: Update destroy**

```c
static void kvbfs_destroy(void *userdata)
{
    (void)userdata;

    printf("KVBFS shutting down...\n");

    /* 同步所有脏 inode */
    inode_sync_all();

    /* 清理缓存 */
    inode_cache_clear();

    printf("KVBFS shutdown complete\n");
}
```

**Step 3: Commit**

```bash
git add src/fuse_ops.c
git commit -m "feat: implement init and destroy FUSE operations"
```

---

### Task 6.3: Final Build and Test

**Step 1: Build in WSL2**

```bash
cd /mnt/d/project/kvbfs
mkdir -p build && cd build
cmake ..
make
```

**Step 2: Test mount**

```bash
mkdir -p /tmp/kvbfs_mnt
export KVBFS_DB_PATH=/tmp/kvbfs_db
./kvbfs /tmp/kvbfs_mnt -f -d
```

**Step 3: Test basic operations (in another terminal)**

```bash
ls /tmp/kvbfs_mnt
touch /tmp/kvbfs_mnt/test.txt
echo "hello" > /tmp/kvbfs_mnt/test.txt
cat /tmp/kvbfs_mnt/test.txt
mkdir /tmp/kvbfs_mnt/subdir
ls -la /tmp/kvbfs_mnt
rm /tmp/kvbfs_mnt/test.txt
rmdir /tmp/kvbfs_mnt/subdir
```

**Step 4: Unmount**

```bash
fusermount -u /tmp/kvbfs_mnt
```

**Step 5: Final commit**

```bash
git add -A
git commit -m "chore: complete KVBFS implementation"
```

---

## Summary

| Phase | Tasks | Description |
|-------|-------|-------------|
| 1 | 1.1-1.4 | Superblock, context, main startup |
| 2 | 2.1 | inode management |
| 3 | 3.1-3.4 | Read-only ops: getattr, lookup, readdir, open, read |
| 4 | 4.1-4.2 | Directory write: mkdir, rmdir |
| 5 | 5.1-5.5 | File write: create, write, unlink, setattr, release |
| 6 | 6.1-6.3 | rename, init/destroy, final test |

Total: 14 tasks
