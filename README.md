# AgentFS (KVBFS)

基于 KV 存储的 FUSE 用户态文件系统，专为 AI Agent 工作区设计。

AgentFS 将传统文件系统语义与 Agent 友好的原语结合：自动版本快照、结构化元数据（xattr）、内容自动索引与语义搜索。Agent 通过标准 POSIX 文件操作与文件系统交互，所有有状态信息以自然的方式被记录和检索。

## 核心特性

| 特性 | 说明 |
|------|------|
| **KV 后端存储** | RocksDB（默认）或 NVMe KV SSD（通过模拟器） |
| **xattr 元数据** | 为任意文件附加键值元数据，支持虚拟 `agentfs.*` 只读命名空间 |
| **自动版本快照** | 每次写入关闭时自动创建 CoW 快照，最多保留 64 个版本 |
| **内容自动索引** | 文件关闭时自动检测文本、分块、生成 embedding 向量 |
| **语义搜索** | 通过虚拟文件 `/.agentfs` 或 ioctl 接口进行自然语言搜索 |
| **变更事件流** | 通过虚拟文件 `/.events` 实时流式获取文件系统变更（JSON Lines） |
| **本地 LLM 推理** | 集成 llama.cpp，支持会话式对话推理（可选） |
| **MCP Server** | 通过 MCP 协议为 AI Agent（如 Claude Code）提供文件系统工具 |
| **FUSE lowlevel** | 高性能低级 FUSE 接口，支持多线程 |

## 快速开始

### 依赖

```bash
# Ubuntu / Debian
sudo apt install libfuse3-dev librocksdb-dev pkg-config cmake build-essential

# Arch Linux
sudo pacman -S fuse3 rocksdb cmake pkgconf

# 可选：用于 xattr 测试
sudo apt install python3
```

### 编译

```bash
# 基础构建（仅文件系统 + xattr + 版本快照）
cmake -B build
make -C build -j$(nproc)

# 带 LLM 推理和语义搜索
cmake -B build \
  -DCFS_LOCAL_LLM=ON \
  -DCFS_MEMORY=ON \
  -DLLAMA_DIR=/path/to/llama.cpp
make -C build -j$(nproc)

# 带测试
cmake -B build -DBUILD_TESTS=ON
make -C build -j$(nproc)
```

### CMake 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `KVBFS_BACKEND` | `rocksdb` | KV 后端：`rocksdb` 或 `nvme` |
| `CFS_LOCAL_LLM` | `OFF` | 启用 llama.cpp 本地 LLM 推理 |
| `CFS_MEMORY` | `OFF` | 启用 embedding 记忆子系统（独立于 `CFS_LOCAL_LLM`，自动查找 llama.cpp） |
| `LLAMA_DIR` | (空) | llama.cpp 源码路径（自动查找头文件和库） |
| `BUILD_TESTS` | `OFF` | 构建测试 |

### 挂载

```bash
# 基本用法
mkdir -p /tmp/kvbfs_mnt
./build/kvbfs /tmp/kvbfs_mnt -f

# 指定数据库路径
KVBFS_DB_PATH=/var/lib/agentfs ./build/kvbfs /tmp/kvbfs_mnt -f

# 后台运行
./build/kvbfs /tmp/kvbfs_mnt

# 卸载
fusermount -u /tmp/kvbfs_mnt
```

### 命令行选项

```
./build/kvbfs <mountpoint> [FUSE options]

FUSE options:
  -f        前台运行（推荐调试时使用）
  -d        调试模式（隐含 -f，打印所有 FUSE 调用）
  -s        单线程模式
  --help    显示帮助
  --version 显示版本
```

### 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `KVBFS_DB_PATH` | `/tmp/kvbfs_data` | RocksDB 数据目录路径 |
| `CFS_MODEL_PATH` | (无，禁用 LLM) | GGUF 格式对话模型路径 |
| `CFS_N_CTX` | `4096` | LLM 上下文窗口大小 |
| `CFS_N_GPU_LAYERS` | `0` | LLM GPU offload 层数 |
| `CFS_MAX_TOKENS` | `512` | 每次推理最大生成 token 数 |
| `CFS_TEMPERATURE` | `0.7` | 采样温度 |
| `CFS_EMBED_MODEL_PATH` | (无，禁用索引) | GGUF 格式 embedding 模型路径 |
| `CFS_EMBED_N_CTX` | `512` | Embedding 上下文窗口大小 |
| `CFS_EMBED_N_GPU_LAYERS` | `0` | Embedding GPU offload 层数 |

## 使用指南

### 基础文件操作

AgentFS 完全兼容 POSIX 文件操作：

```bash
# 创建文件和目录
echo "hello" > /tmp/kvbfs_mnt/file.txt
mkdir /tmp/kvbfs_mnt/workspace

# 读写
cat /tmp/kvbfs_mnt/file.txt
echo "more data" >> /tmp/kvbfs_mnt/file.txt

# 目录操作
ls -la /tmp/kvbfs_mnt/
cp /tmp/kvbfs_mnt/file.txt /tmp/kvbfs_mnt/backup.txt
mv /tmp/kvbfs_mnt/backup.txt /tmp/kvbfs_mnt/workspace/

# 符号链接和硬链接
ln -s file.txt /tmp/kvbfs_mnt/link.txt
ln /tmp/kvbfs_mnt/file.txt /tmp/kvbfs_mnt/hardlink.txt
```

### xattr 元数据

为文件附加任意键值元数据：

```python
import os

path = "/tmp/kvbfs_mnt/file.txt"

# 设置
os.setxattr(path, "user.author", b"alice")
os.setxattr(path, "user.tags", b"draft,important")

# 读取
author = os.getxattr(path, "user.author")  # b"alice"

# 列出
attrs = os.listxattr(path)  # ["user.author", "user.tags"]

# 删除
os.removexattr(path, "user.tags")
```

#### 虚拟 agentfs.* 命名空间

以 `agentfs.` 开头的 xattr 是只读的，由文件系统动态计算：

```python
import os, json

path = "/tmp/kvbfs_mnt/file.txt"

# 获取当前版本号
ver = os.getxattr(path, "agentfs.version").decode()
print(f"版本: {ver}")

# 获取所有版本的元数据（JSON 数组）
raw = os.getxattr(path, "agentfs.versions").decode()
versions = json.loads(raw)
for v in versions:
    print(f"  版本 {v['version']}: size={v['size']} mtime={v['mtime']}")
```

可用的虚拟 xattr：

| 属性 | 类型 | 说明 |
|------|------|------|
| `agentfs.version` | string | 当前版本号（十进制） |
| `agentfs.versions` | JSON | 所有版本的元数据数组 |

### 自动版本快照

每次文件被写入并关闭后，AgentFS 自动保存一个 copy-on-write 快照：

```bash
echo "v1" > /tmp/kvbfs_mnt/doc.txt    # → 版本 1
echo "v2" > /tmp/kvbfs_mnt/doc.txt    # → 版本 2
echo "v3" > /tmp/kvbfs_mnt/doc.txt    # → 版本 3
```

- 最多保留 **64 个版本**（超出后自动清理最旧的版本）
- 版本数据在文件被删除时自动清理
- 通过 `agentfs.version` 和 `agentfs.versions` xattr 查询版本信息
- 空文件不创建快照

### 语义搜索

需要编译时启用 `CFS_MEMORY=ON` 并在运行时设置 `CFS_EMBED_MODEL_PATH`。

推荐使用 [nomic-embed-text](https://huggingface.co/nomic-ai/nomic-embed-text-v1.5-GGUF) GGUF 模型。

#### 自动索引

文件写入关闭时自动触发索引：

- 检测文件内容是否为文本（前 512 字节无 null byte）
- 二进制文件自动跳过
- 文本按 ~1600 字节分块，块间 ~200 字节重叠，优先在换行处断句
- 每次写入重建该文件的全部 embedding（先删旧的再生成新的）
- 索引在后台异步执行，不阻塞文件操作

#### 跳过索引

设置 `agentfs.noindex` xattr 可让文件跳过自动索引：

```python
import os
os.setxattr("/tmp/kvbfs_mnt/secret.txt", "agentfs.noindex", b"1")
```

#### 通过 /.agentfs 搜索

根目录下的虚拟文件 `/.agentfs` 提供 JSON 搜索接口：

```python
import os, json

# 打开控制文件，写入查询，读取结果
fd = os.open("/tmp/kvbfs_mnt/.agentfs", os.O_RDWR)
os.write(fd, b"meeting notes about project deadline")
os.lseek(fd, 0, os.SEEK_SET)
data = os.read(fd, 65536).decode()
os.close(fd)

result = json.loads(data)
print(json.dumps(result, indent=2, ensure_ascii=False))
```

返回格式：

```json
{
  "status": "ok",
  "results": [
    {
      "score": 0.8234,
      "ino": 42,
      "seq": 0,
      "path": "/workspace/meeting.md",
      "summary": "匹配的文本片段..."
    }
  ]
}
```

不写入直接读取会返回使用说明：

```bash
cat /tmp/kvbfs_mnt/.agentfs
# {"status":"ready","usage":"Write a search query, then read results."}
```

### 变更事件流 (`.events`)

根目录下的虚拟文件 `/.events` 提供实时文件系统变更通知（JSON Lines 格式）：

```bash
# 打开 .events 后，所有后续文件操作会产生事件
cat /tmp/kvbfs_mnt/.events
```

每行是一个 JSON 对象：

```json
{"seq":1,"type":"create","ino":42,"path":"readme.md","ts":1708300000}
{"seq":2,"type":"write","ino":42,"path":"","ts":1708300001}
```

支持的事件类型：`create`, `write`, `unlink`, `mkdir`, `rmdir`, `rename`, `setattr`, `setxattr`, `removexattr`, `link`

特性：
- 256 KB 环形缓冲区，溢出时自动丢弃最旧事件
- 每次 `open()` 从当前位置开始读取（不会看到打开之前的旧事件）
- 只读文件，写入返回 `EACCES`
- 支持 `poll()` 异步通知

#### 通过 ioctl 搜索

也可通过 ioctl 接口进行搜索（适合 C/C++ 程序）：

```c
#include <sys/ioctl.h>

// 在任意已打开的 KVBFS 文件上执行
struct cfs_mem_query query = {0};
strncpy(query.query_text, "search terms", sizeof(query.query_text) - 1);
query.top_k = 5;

ioctl(fd, CFS_IOC_MEM_SEARCH, &query);

for (int i = 0; i < query.n_results; i++) {
    printf("%.4f %s: %s\n",
           query.results[i].score,
           query.results[i].path,
           query.results[i].summary);
}
```

测试工具 `test_mem_ioctl` 可用于命令行搜索：

```bash
./build/tests/test_mem_ioctl /tmp/kvbfs_mnt/anyfile "search query" 5
```

### LLM 对话推理

需要编译时启用 `CFS_LOCAL_LLM=ON` 并在运行时设置 `CFS_MODEL_PATH`。

```bash
CFS_MODEL_PATH=/path/to/chat-model.gguf \
CFS_EMBED_MODEL_PATH=/path/to/embed-model.gguf \
  ./build/kvbfs /tmp/kvbfs_mnt -f
```

对话通过 `/sessions/` 目录下的文本文件进行：

```bash
# 开始新对话
echo "User: Hello, who are you?" > /tmp/kvbfs_mnt/sessions/chat1

# 等待 Assistant 回复（异步生成）
sleep 3
cat /tmp/kvbfs_mnt/sessions/chat1
# User: Hello, who are you?
# Assistant: I'm an AI assistant...

# 继续对话（必须用 >> 追加）
echo "User: Tell me about FUSE filesystems" >> /tmp/kvbfs_mnt/sessions/chat1
```

通过 ioctl 查询生成状态：

```c
struct cfs_status st;
ioctl(fd, CFS_IOC_STATUS, &st);
// st.generating == 1 表示正在生成中
```

## 运行测试

```bash
# 构建带测试的版本
cmake -B build -DBUILD_TESTS=ON -DCFS_LOCAL_LLM=ON -DCFS_MEMORY=ON \
  -DLLAMA_DIR=/path/to/llama.cpp
make -C build -j$(nproc)

# 单元测试（test_kv_store + test_inode）
cd build && ctest --output-on-failure

# E2E 集成测试（48 项）
bash tests/test_kvbfs.sh /tmp/kvbfs_mnt ./build/kvbfs
```

E2E 测试覆盖：

| 范围 | 测试编号 | 项目数 |
|------|----------|--------|
| 基础 POSIX 操作 | 1-21 | 23 |
| 符号链接 / 硬链接 | 22-29 | 8 |
| xattr 元数据 | 30-35 | 6 |
| 版本快照 | 36-40 | 5 |
| .agentfs 虚拟文件 | 41-46 | 6 |
| .events 变更通知 | 47-51 | 5 |

## 架构

### 组件结构

```
┌─────────────────────────────────────────────────┐
│                  FUSE Client                     │
│        (ls, cat, echo, python, Agent...)         │
└──────────────────────┬──────────────────────────┘
                       │ FUSE lowlevel
┌──────────────────────┴──────────────────────────┐
│                 fuse_ops.c                       │
│  lookup · getattr · readdir · read · write ...   │
│  xattr · version · .agentfs virtual file         │
├──────────────────────────────────────────────────┤
│          inode.c         │      version.c        │
│   缓存 + refcount +      │  CoW 快照 + 版本管理   │
│   延迟删除               │  (最多 64 版本)        │
├──────────────────────────┼───────────────────────┤
│          kv_store.c  (抽象层)                     │
├──────────────┬───────────────────────────────────┤
│ kv_rocksdb.c │  kv_nvme.c (TCP → nvme-kv-sim)   │
└──────────────┴───────────────────────────────────┘

┌─────────────────┐  ┌──────────────────────────┐
│    llm.c        │  │        mem.c             │
│  对话推理        │  │  embedding 索引 + 搜索    │
│  后台 worker     │  │  后台 worker + 分块       │
│  会话文件监控    │  │  /.agentfs 虚拟文件       │
└─────────────────┘  └──────────────────────────┘
```

### KV Key 命名空间

所有数据存储在扁平 KV 空间中，通过 key 前缀区分：

| Key 格式 | 值 | 说明 |
|----------|-----|------|
| `sb` | `struct kvbfs_super` | 超级块 |
| `i:<ino>` | `struct kvbfs_inode` | inode 元数据 |
| `d:<parent_ino>:<name>` | `uint64_t child_ino` | 目录项 |
| `b:<ino>:<block_idx>` | 4096 字节数据 | 文件数据块 |
| `x:<ino>:<xattr_name>` | 任意字节 | 扩展属性 |
| `vc:<ino>` | `uint64_t` | 版本计数器 |
| `vm:<ino>:<ver>` | `struct kvbfs_version_meta` | 版本元数据 |
| `vb:<ino>:<ver>:<block>` | 4096 字节数据 | 版本数据块 |
| `m:v:<ino>:<seq>` | `float[n_embd]` | Embedding 向量 |
| `m:t:<ino>:<seq>` | 文本 | 文本块原文 |
| `m:h:<ino>:<seq>` | `struct mem_header` | Embedding 头信息 |
| `m:seq:<ino>` | `uint32_t` | 每 inode 序列计数器 |

### 文件系统常量

| 常量 | 值 | 说明 |
|------|-----|------|
| `KVBFS_BLOCK_SIZE` | 4096 | 数据块大小 |
| `KVBFS_ROOT_INO` | 1 | 根目录 inode 号 |
| `KVBFS_KEY_MAX` | 512 | KV key 最大长度 |
| `KVBFS_MAX_VERSIONS` | 64 | 每文件最大版本数 |
| `AGENTFS_CTL_INO` | 0xFFFFFFFFFFFFFF | .agentfs 虚拟 inode |
| `AGENTFS_EVENTS_INO` | 0xFFFFFFFFFFFFFE | .events 虚拟 inode |

### MCP Server

AgentFS 提供 MCP Server，让 Claude Code 等 AI Agent 通过 MCP 协议访问文件系统功能：

```bash
# 安装依赖
cd cfs && pip install -r requirements.txt

# 启动（stdio transport，适用于 Claude Code 集成）
KVBFS_MOUNT=/tmp/kvbfs_mnt python cfs/mcp_server.py
```

提供以下 MCP 工具：

| 工具 | 说明 |
|------|------|
| `semantic_search` | 语义搜索文件内容 |
| `get_file_versions` | 获取文件版本历史 |
| `get_file_metadata` | 获取文件 xattr 元数据 |
| `set_file_metadata` | 设置文件 xattr |
| `get_events` | 获取最近变更事件 |
| `list_files` | 列出目录内容及元数据 |

Claude Code 集成配置（`.claude/mcp.json`）：

```json
{
  "mcpServers": {
    "agentfs": {
      "command": "python",
      "args": ["cfs/mcp_server.py"],
      "env": {
        "KVBFS_MOUNT": "/tmp/kvbfs_mnt"
      }
    }
  }
}
```

## NVMe KV 模拟器

`sim/` 目录包含一个 NVMe KV SSD 的 TCP 模拟器，用于无硬件开发测试：

```bash
# 启动模拟器
./build/sim/nvme-kv-sim --port 9527

# 使用 NVMe 后端编译 KVBFS
cmake -B build -DKVBFS_BACKEND=nvme
make -C build -j$(nproc)

# 挂载（自动连接 localhost:9527）
./build/kvbfs /tmp/kvbfs_mnt -f
```

模拟器使用内存中的哈希表存储数据，支持 Store / Retrieve / Delete / Exist / List 操作。

## 项目结构

```
KVBFS/
├── CMakeLists.txt          # 构建配置
├── README.md               # 本文档
├── src/
│   ├── main.c              # 入口，环境变量解析，FUSE session 管理
│   ├── kvbfs.h             # 核心类型、常量、KV key 格式、ioctl 定义
│   ├── fuse_ops.c          # 全部 FUSE lowlevel 操作实现
│   ├── inode.h / inode.c   # inode 缓存、引用计数、延迟删除
│   ├── context.h / context.c # 全局上下文初始化与销毁
│   ├── super.h / super.c   # 超级块持久化
│   ├── version.h / version.c # 版本快照 (CoW)
│   ├── kv_store.h / kv_store.c # KV 存储抽象层
│   ├── kv_rocksdb.c        # RocksDB 后端实现
│   ├── kv_nvme.c           # NVMe TCP 客户端后端
│   ├── llm.h / llm.c       # LLM 对话推理子系统
│   ├── mem.h / mem.c       # Embedding 记忆子系统
│   ├── events.h / events.c # 变更事件通知子系统
│   ├── utils.c             # 工具函数
│   ├── uthash.h            # 内嵌 hash 表库
│   └── nvme_kv_proto.h     # NVMe KV 协议定义
├── sim/                    # NVMe KV 模拟器
├── cfs/                    # CFS-Local Python 守护进程、SDK 和 MCP Server
├── tests/
│   ├── test_kvbfs.sh       # E2E 集成测试（48 项）
│   ├── test_kv_store.c     # KV 存储单元测试
│   ├── test_inode.c        # inode 子系统单元测试（6 项）
│   └── test_mem_ioctl.c    # ioctl 搜索命令行工具
├── scripts/
│   ├── mount.sh            # 挂载脚本
│   └── umount.sh           # 卸载脚本
└── docs/plans/             # 设计文档
```

## 许可证

[待定]
