# KVBFS 设计文档

## 概述

KVBFS 是一个基于 NVMe KV 语义的用户态文件系统，通过 FUSE 实现。

**核心特点：**
- 用户态实现，基于 FUSE
- 底层使用 KV 存储（当前 RocksDB，未来可对接 NVMe KV SSD）
- 预留 eBPF 快路径扩展点

**技术选型：**
- 语言：C
- 构建：CMake
- 依赖：libfuse3, RocksDB, uthash, pthread
- 开发环境：Windows
- 目标平台：Linux (WSL2/VM)

## 架构

```
┌─────────────────────────────────────────────┐
│                用户应用程序                   │
└─────────────────┬───────────────────────────┘
                  │ POSIX API
                  ▼
┌─────────────────────────────────────────────┐
│                Linux VFS                     │
└─────────────────┬───────────────────────────┘
                  │ FUSE 协议
                  ▼
┌─────────────────────────────────────────────┐
│           FUSE Operations Layer             │
│   (inode 缓存 / 锁管理 / 目录逻辑)            │
└─────────────────┬───────────────────────────┘
                  ▼
┌─────────────────────────────────────────────┐
│            KV Storage Layer                 │
│        (RocksDB / 未来 NVMe KV)             │
└─────────────────────────────────────────────┘
```

## KV 数据模型

| 类型 | Key 格式 | Value 内容 |
|------|----------|------------|
| 超级块 | `sb` | 文件系统元信息（魔数、版本、根 inode 号） |
| inode | `i:<ino>` | 元数据结构体（类型、大小、权限、时间戳、nlink） |
| 目录项 | `d:<parent_ino>:<name>` | 子项的 inode 号 |
| 数据块 | `b:<ino>:<block_idx>` | 文件数据（固定 4KB 块） |
| inode 分配 | `next_ino` | 下一个可用 inode 号 |

**示例：**
```
文件 /hello.txt (ino=2, 大小 5000 字节):

Key                    Value
─────────────────────────────────────
i:2                    {type=FILE, size=5000, mode=0644, ...}
d:1:hello.txt          2
b:2:0                  [前 4096 字节数据]
b:2:1                  [后 904 字节数据]
```

## 核心数据结构

```c
/* 配置常量 */
#define KVBFS_BLOCK_SIZE    4096
#define KVBFS_MAGIC         0x4B564246  /* "KVBF" */
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
    rocksdb_t *db;
    struct kvbfs_inode_cache *icache;
    pthread_mutex_t icache_lock;
    pthread_mutex_t alloc_lock;
};
```

**锁策略：**
- `icache_lock`: 保护缓存哈希表的插入/删除
- 每个 inode 独立 `rwlock`: 读共享，写独占
- `alloc_lock`: 保护 inode 号分配

## FUSE 操作接口

使用 `fuse_lowlevel_ops` 低级接口：

| 操作 | 功能 |
|------|------|
| `init` | 打开 RocksDB，加载超级块 |
| `destroy` | 刷写缓存，关闭 RocksDB |
| `lookup` | 根据父目录+名字查找 inode |
| `getattr` | 返回 inode 元数据 |
| `setattr` | 修改权限、大小、时间戳 |
| `readdir` | 遍历目录项 |
| `mkdir` | 创建目录 |
| `rmdir` | 删除空目录 |
| `create` | 创建并打开文件 |
| `unlink` | 删除文件 |
| `open` | 打开文件 |
| `release` | 关闭文件 |
| `read` | 读数据块 |
| `write` | 写数据块 |
| `rename` | 重命名/移动 |

**暂不实现：** link, symlink, mknod, xattr 系列

## 项目结构

```
kvbfs/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── main.c
│   ├── kvbfs.h
│   ├── fuse_ops.c
│   ├── kv_store.c
│   ├── kv_store.h
│   ├── kv_rocksdb.c
│   ├── inode.c
│   ├── inode.h
│   └── utils.c
├── tests/
│   ├── CMakeLists.txt
│   ├── test_kv_store.c
│   ├── test_inode.c
│   └── test_integration.c
├── scripts/
│   ├── mount.sh
│   └── umount.sh
└── docs/
    └── plans/
```

## 开发阶段

### 阶段 1：基础框架
- 搭建 CMake 项目结构
- 实现 KV 存储抽象层 + RocksDB 后端
- 实现超级块初始化/加载

### 阶段 2：只读文件系统
- 实现 lookup、getattr、readdir、open、read
- 手动写入测试数据验证

### 阶段 3：目录写操作
- 实现 mkdir、rmdir、inode 分配
- 实现目录项增删

### 阶段 4：文件写操作
- 实现 create、unlink、write、setattr
- 实现数据块分配和写入

### 阶段 5：完善功能
- 实现 rename
- 完善错误处理
- 添加测试用例

### 阶段 6（未来）：eBPF 快路径
- 设计 eBPF 读路径
- 用户态与 eBPF 共享数据结构

## 依赖

- libfuse3: FUSE 用户态库
- rocksdb: KV 存储后端
- uthash: 头文件单文件哈希表
- pthread: 多线程支持
