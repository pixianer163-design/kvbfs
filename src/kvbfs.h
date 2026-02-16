#ifndef KVBFS_H
#define KVBFS_H

#define FUSE_USE_VERSION 35

#include <fuse_lowlevel.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include "uthash.h"

#ifdef CFS_LOCAL_LLM
#include "llm.h"
#endif

#ifdef CFS_MEMORY
#include "mem.h"
#endif

/* 配置常量 */
#define KVBFS_BLOCK_SIZE    4096
#define KVBFS_MAGIC         0x4B564246  /* "KVBF" */
#define KVBFS_VERSION       1
#define KVBFS_ROOT_INO      1

/* 超级块 */
struct kvbfs_super {
    uint32_t magic;
    uint32_t version;
    uint64_t next_ino;
};

/* inode 结构 (持久化到 KV) */
struct kvbfs_inode {
    uint64_t ino;
    uint32_t mode;
    uint32_t nlink;
    uint64_t size;
    uint64_t blocks;
    struct timespec atime;
    struct timespec mtime;
    struct timespec ctime;
};

/* 内存中的 inode 缓存项 */
struct kvbfs_inode_cache {
    struct kvbfs_inode inode;
    pthread_rwlock_t lock;
    uint64_t refcount;
    bool dirty;
    UT_hash_handle hh;
};

/* 文件系统全局上下文 */
struct kvbfs_ctx {
    void *db;                           /* KV 存储句柄 */
    struct kvbfs_inode_cache *icache;   /* inode 缓存哈希表 */
    pthread_mutex_t icache_lock;        /* 缓存表锁 */
    pthread_mutex_t alloc_lock;         /* inode 分配锁 */
    struct kvbfs_super super;           /* 超级块 */

#ifdef CFS_LOCAL_LLM
    struct llm_ctx llm;                 /* LLM 推理子系统 */
    uint64_t sessions_ino;              /* /sessions 目录 inode 号 */
#endif

#ifdef CFS_MEMORY
    struct mem_ctx mem;                 /* Memory/embedding subsystem */
#endif
};

/* 全局上下文 */
extern struct kvbfs_ctx *g_ctx;

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

#ifdef CFS_LOCAL_LLM
#include <sys/ioctl.h>

struct cfs_status {
    uint32_t generating;
    uint32_t reserved;
};

#define CFS_IOC_MAGIC   'C'
#define CFS_IOC_STATUS  _IOR(CFS_IOC_MAGIC, 1, struct cfs_status)
#define CFS_IOC_CANCEL  _IO(CFS_IOC_MAGIC, 2)

#ifdef CFS_MEMORY
#define CFS_MEM_MAX_RESULTS  16
#define CFS_MEM_SUMMARY_LEN  512

struct cfs_mem_result {
    uint64_t ino;
    uint32_t seq;
    float    score;
    char     summary[CFS_MEM_SUMMARY_LEN];
};

struct cfs_mem_query {
    char     query_text[512];
    int      top_k;
    int      n_results;
    struct cfs_mem_result results[CFS_MEM_MAX_RESULTS];
};

#define CFS_IOC_MEM_SEARCH  _IOWR(CFS_IOC_MAGIC, 10, struct cfs_mem_query)
#endif /* CFS_MEMORY */
#endif /* CFS_LOCAL_LLM */

#endif /* KVBFS_H */
