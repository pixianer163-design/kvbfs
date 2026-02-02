#ifndef KVBFS_H
#define KVBFS_H

#define FUSE_USE_VERSION 35

#include <fuse_lowlevel.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "uthash.h"

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
};

/* 全局上下文 */
extern struct kvbfs_ctx *g_ctx;

#endif /* KVBFS_H */
