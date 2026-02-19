# AgentFS 版本虚拟目录设计

**日期**: 2026-02-19
**状态**: 已批准

## 目标

通过 `/.versions/<path>/<N>` 虚拟目录树，让 Agent 和用户能用标准 POSIX 操作读取文件的历史版本内容，并通过 `cp` 恢复旧版本。

## 背景

AgentFS 已实现 CoW 版本快照（`version.c`），每次文件写入关闭后自动保存，最多 64 版本。但目前只能通过 `agentfs.versions` xattr 查看版本元数据，无法读取旧版本内容。

## 设计

### 接口

```bash
# 读旧版本内容
cat /.versions/workspace/notes/doc.txt/3

# 列出所有版本
ls /.versions/workspace/notes/doc.txt/

# 恢复旧版本（纯 POSIX，cp 写入当前文件并产生新快照）
cp /.versions/workspace/notes/doc.txt/3 /workspace/notes/doc.txt
```

### 虚拟 inode 命名空间

```
0xFFFFFFFFFFFFFF   → AGENTFS_CTL_INO      (.agentfs，已有)
0xFFFFFFFFFFFFFE   → AGENTFS_EVENTS_INO   (.events，已有)
0xFFFFFFFFFFFFFD   → AGENTFS_VERSIONS_INO (.versions 根目录，新增，固定)
0xC000000000000001 起  → 动态分配的虚拟中间目录 / 版本文件 inode
```

### 数据结构（src/vfs_versions.h）

```c
struct vtree_node {
    uint64_t vino;            // 虚拟 inode（主键）
    uint64_t real_ino;        // 对应的真实 inode
    int      is_version_file; // 1=叶子版本文件, 0=中间目录
    uint64_t version;         // 版本号（仅 is_version_file=1 时有效）
    UT_hash_handle hh;        // 按 vino 查找
};

struct vtree_lookup_entry {
    char     key[32];         // sprintf("%llu:%s", parent_vino, name)
    uint64_t vino;
    UT_hash_handle hh;        // 按 (parent_vino, name) 查找
};

struct vtree_ctx {
    struct vtree_node         *by_ino;    // hash by vino
    struct vtree_lookup_entry *by_parent; // hash by (parent_vino:name)
    uint64_t                   next_vino; // 分配计数器，从 AGENTFS_VDIR_BASE 开始
    pthread_mutex_t            lock;
};
```

### 路径解析流程

以 `cat /.versions/workspace/notes/doc.txt/3` 为例，FUSE 逐级 lookup：

```
lookup(".versions", parent=root_ino)
  → 返回固定 AGENTFS_VERSIONS_INO

lookup("workspace", parent=VERSIONS_INO)
  → dirent_lookup(root_ino, "workspace") 得到 real_ino=10
  → 分配 vino=0xC...01；存 {vino, real_ino=10, is_version_file=0}
  → lookup_map["VERSIONS_INO:workspace"] = vino

lookup("notes", parent=vino_1)
  → dirent_lookup(10, "notes") 得到 real_ino=20
  → 分配 vino=0xC...02

lookup("doc.txt", parent=vino_2)
  → dirent_lookup(20, "doc.txt") 得到 real_ino=42
  → 分配 vino=0xC...03；此节点是"版本列表目录"

lookup("3", parent=vino_3)
  → 检查 KV key "vm:42:3" 存在
  → 分配 vino=0xC...04；{real_ino=42, is_version_file=1, version=3}

read(vino_4)
  → 读 KV "vb:42:3:0", "vb:42:3:1", ... 返回内容
```

**关键点**：每个节点存 `real_ino`，readdir 直接用 `d:<real_ino>:<name>` 枚举子项，无需重新解析路径字符串。

### 受影响的 FUSE 操作

| 操作 | 改动 |
|------|------|
| `kvbfs_lookup` | 识别 `.versions`；虚拟 inode 的子级 lookup 走 vtree |
| `kvbfs_getattr` | 虚拟目录返回 `S_IFDIR 0555`；版本文件返回版本的 size/mtime |
| `kvbfs_readdir` | 虚拟目录枚举真实 KV 子目录；版本列表目录枚举 `vm:<ino>:*` 键 |
| `kvbfs_open` | 版本文件只允许 `O_RDONLY` |
| `kvbfs_read` | 读 `vb:<real_ino>:<ver>:<block_idx>` |
| `kvbfs_release` | 版本文件 handle 释放（无状态，直接返回） |

write、create、unlink、xattr 等写操作对虚拟树返回 `EACCES` / `ENOTSUP`。

### 错误处理

| 场景 | 返回 |
|------|------|
| 路径分量在真实 KV 中不存在 | `ENOENT` |
| 请求版本号不存在（`vm:<ino>:N` 无此 key） | `ENOENT` |
| 对版本文件执行 write/create/unlink | `EACCES` / `ENOTSUP` |
| 版本列表目录 readdir 时文件已被删除 | 空目录（版本数据随 unlink 清理） |
| vtree 内存分配失败 | `ENOMEM` |

## 测试（Tests 52–57）

| 编号 | 内容 |
|------|------|
| 52 | `.versions` 出现在根目录列表中 |
| 53 | 写入文件后 `.versions/<file>/` 目录存在且可列出版本号 |
| 54 | `cat /.versions/<file>/1` 内容与第一次写入一致 |
| 55 | `cat /.versions/<file>/2` 内容与第二次写入一致（当前版本不同） |
| 56 | `cp /.versions/<file>/1 <file>` 可以恢复旧版本（版本号+1） |
| 57 | `.versions` 下的文件不可写（write 返回 EACCES） |

## 文件变更清单

```
新增：
  src/vfs_versions.h
  src/vfs_versions.c

修改：
  src/kvbfs.h          ← 新增常量、struct kvbfs_ctx 加 vtree_ctx
  src/context.h/c      ← vtree_init / vtree_destroy
  src/fuse_ops.c       ← lookup/getattr/readdir/open/read/release
  CMakeLists.txt       ← KVBFS_SOURCES 加入 vfs_versions.c
  tests/test_kvbfs.sh  ← Tests 52–57
  .gitignore           ← 新增（build*/, __pycache__/, .venv/）
```
