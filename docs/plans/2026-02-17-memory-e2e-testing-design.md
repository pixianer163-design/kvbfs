# 记忆子系统 E2E 测试设计

## 目标

为已实现的记忆子系统（embedding 存储、语义搜索、上下文压缩）添加端到端测试，
确保功能在真实模型下正确工作。

## 前提

- Embedding 模型: `nomic-embed-text-v1.5.Q4_K_M.gguf` (768 维)
- Chat 模型: `qwen2.5-0.5b-instruct-q4_k_m.gguf`
- 构建标志: `CFS_LOCAL_LLM=ON CFS_MEMORY=ON`

## 新增文件

- `tests/test_mem_ioctl.c` — ioctl 搜索测试工具
- 修改 `tests/CMakeLists.txt` — 添加构建目标
- 修改 `cfs/tests/test_local_llm.sh` — 增加记忆测试段

## 测试用例

### A: Embedding 存储与语义搜索

1. 写入多主题对话到 `/sessions/mem_topics`
2. 等待推理 + embedding 完成
3. 用 `test_mem_ioctl` 发送 ioctl 查询 "programming"
4. 验证: 返回结果 > 0，最高分结果包含编程相关内容

### B: 多 Session 记忆

1. 两个 session 写入不同主题
2. ioctl 查询某一主题
3. 验证: 能跨 session 找到相关记忆

### C: 上下文压缩

1. 向 session 写入大量对话（接近 n_ctx 上限）
2. 触发推理（release 时检测并压缩）
3. 验证: 文件包含 `[Context Summary]` 标记

## test_mem_ioctl.c 接口

```
用法: test_mem_ioctl <session_file> <query_text> [top_k]
输出: 每行一个结果，格式: score<TAB>ino:seq<TAB>summary
退出码: 0=有结果, 1=无结果, 2=错误
```

## 实现步骤

1. 创建 `tests/test_mem_ioctl.c`
2. 修改 `tests/CMakeLists.txt` 添加条件编译目标
3. 在 `test_local_llm.sh` 中添加记忆测试段（启用 CFS_EMBED_MODEL_PATH）
4. 运行测试验证
