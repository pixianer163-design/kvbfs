# CFS 本地模型直接集成设计

## 目标

将 llama.cpp 直接集成到 KVBFS FUSE 进程中，本地模型推理不再经过 Python daemon 中转。
外部模型（OpenAI API 等）继续通过独立进程访问。

## 架构

```
全局配置: CFS_BACKEND=local | external

local 路径:
  FUSE 进程启动
    → 加载 llama.cpp 模型 (常驻内存)
    → 启动推理线程 (消费任务队列)
    → FUSE 主循环

  write("/sessions/chat1", "User: hello\n")
    → kvbfs_write() 正常写入数据
    → kvbfs_release() 检测 sessions/ 目录下文件
    → 检测未回复的 User: 消息 → 推入任务队列
    → 推理线程: 创建哨兵 → llama 生成 → 追加 Assistant: → 删除哨兵

external 路径:
  FUSE 进程不加载模型，行为与当前一致
  Python daemon 通过 inotify 监控并调用外部 API
```

## 新增文件

- `src/llm.h` — 推理子系统接口声明
- `src/llm.c` — 推理线程、任务队列、llama.cpp 调用

## 关键设计

### 1. 任务队列

```c
struct llm_task {
    uint64_t ino;           // session 文件 inode
    struct llm_task *next;
};

struct llm_ctx {
    struct llama_model *model;
    struct llama_context *ctx;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    struct llm_task *head, *tail;
    int shutdown;
};
```

### 2. 触发时机

在 `kvbfs_release()` 中触发（而非 write），原因：
- write 可能被 split 成多次调用，release 保证文件写入完成
- 只对 sessions/ 目录下的文件触发
- 检测文件内容末尾是否有未回复的 `User:` 消息

### 3. 推理线程工作流

1. 从队列取 task
2. 通过 KV 读取文件全部内容
3. 解析对话历史，构建 prompt
4. 创建哨兵文件 `.generating.<ino>`
5. 调用 llama.cpp 生成
6. 追加 `Assistant: <response>\n` 到文件
7. 删除哨兵文件

### 4. sessions 目录识别

在 `kvbfs_ctx` 中记录 sessions 目录的 inode 号。
`release()` 时检查文件的父目录是否为 sessions 目录。
简化方案：启动时查找或创建 `/sessions` 目录，记住其 ino。

### 5. CMake 集成

```cmake
option(CFS_LOCAL_LLM "Enable local LLM via llama.cpp" OFF)

if(CFS_LOCAL_LLM)
    find_package(llama REQUIRED)  # 或 pkg-config
    list(APPEND KVBFS_SOURCES src/llm.c)
    target_link_libraries(kvbfs llama)
    target_compile_definitions(kvbfs PRIVATE CFS_LOCAL_LLM=1)
endif()
```

未启用时，`release()` 中的触发代码通过 `#ifdef` 跳过。

### 6. 配置

环境变量：
- `CFS_MODEL_PATH` — GGUF 模型文件路径
- `CFS_N_CTX` — 上下文窗口大小 (默认 4096)
- `CFS_N_GPU_LAYERS` — GPU offload 层数 (默认 0)
- `CFS_MAX_TOKENS` — 最大生成 token 数 (默认 512)
- `CFS_TEMPERATURE` — 采样温度 (默认 0.7)

## 实现步骤

1. 创建 `src/llm.h` 和 `src/llm.c`
2. 实现任务队列 + 推理线程框架
3. 集成 llama.cpp 模型加载和推理
4. 修改 `kvbfs_ctx` 增加 `llm_ctx` 和 `sessions_ino`
5. 修改 `context.c` 初始化/销毁 LLM 子系统
6. 修改 `fuse_ops.c` 的 `release()` 触发推理
7. 修改 `CMakeLists.txt` 添加条件编译
8. 修改 `main.c` 读取环境变量配置
