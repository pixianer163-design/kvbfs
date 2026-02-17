#!/bin/bash
# CFS 本地模型端到端测试 (LLM + Memory)
# 用法: ./test_local_llm.sh [chat_model_path] [embed_model_path]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MODEL=${1:-$HOME/models/qwen2.5-0.5b-instruct-q4_k_m.gguf}
EMBED_MODEL=${2:-$HOME/models/nomic-embed-text-v1.5.Q4_K_M.gguf}
KVBFS_BIN="$SCRIPT_DIR/../../build/kvbfs"
MEM_IOCTL_BIN="$SCRIPT_DIR/../../build/tests/test_mem_ioctl"
MOUNT=/tmp/kvbfs_e2e_mount
DATA=/tmp/kvbfs_e2e_data

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

pass=0
fail=0

check() {
    if eval "$2"; then
        echo -e "  ${GREEN}PASS${NC}: $1"
        pass=$((pass + 1))
    else
        echo -e "  ${RED}FAIL${NC}: $1"
        fail=$((fail + 1))
    fi
}

wait_response() {
    local file=$1
    local expect_count=${2:-1}
    for i in $(seq 1 90); do
        local count
        count=$(grep -c "^Assistant:" "$file" 2>/dev/null || true)
        count=${count:-0}
        if [ "$count" -ge "$expect_count" ]; then
            return 0
        fi
        sleep 1
    done
    return 1
}

cleanup() {
    echo -e "\n${CYAN}清理...${NC}"
    kill "$KVBFS_PID" 2>/dev/null; wait "$KVBFS_PID" 2>/dev/null || true
    fusermount3 -u "$MOUNT" 2>/dev/null || fusermount -u "$MOUNT" 2>/dev/null || true
    rm -rf "$MOUNT" "$DATA"
}
trap cleanup EXIT

# ── 准备 ──────────────────────────────────────────────

echo -e "${CYAN}=== CFS 本地 LLM + Memory 端到端测试 ===${NC}"
echo "Chat 模型: $MODEL"
echo "Embed 模型: $EMBED_MODEL"
echo "二进制: $KVBFS_BIN"

if [ ! -f "$MODEL" ]; then
    echo -e "${RED}错误: Chat 模型文件不存在: $MODEL${NC}"
    exit 1
fi

if [ ! -x "$KVBFS_BIN" ]; then
    echo -e "${RED}错误: kvbfs 未编译或不可执行: $KVBFS_BIN${NC}"
    exit 1
fi

# 检查 embed 模型和 ioctl 工具
MEM_ENABLED=0
if [ -f "$EMBED_MODEL" ] && [ -x "$MEM_IOCTL_BIN" ]; then
    MEM_ENABLED=1
    echo "Memory 测试: 已启用"
else
    echo "Memory 测试: 跳过 (缺少 embed 模型或 test_mem_ioctl)"
    [ ! -f "$EMBED_MODEL" ] && echo "  - 缺少: $EMBED_MODEL"
    [ ! -x "$MEM_IOCTL_BIN" ] && echo "  - 缺少: $MEM_IOCTL_BIN"
fi

rm -rf "$MOUNT" "$DATA"
mkdir -p "$MOUNT" "$DATA"

# ── 启动 kvbfs ────────────────────────────────────────

echo -e "\n${CYAN}[1] 启动 kvbfs + 本地 LLM${NC}"

# 构建环境变量
KVBFS_ENV=(
    KVBFS_DB_PATH="$DATA"
    CFS_MODEL_PATH="$MODEL"
    CFS_N_CTX=2048
    CFS_MAX_TOKENS=128
    CFS_TEMPERATURE=0.3
)
if [ "$MEM_ENABLED" -eq 1 ]; then
    KVBFS_ENV+=(
        CFS_EMBED_MODEL_PATH="$EMBED_MODEL"
        CFS_EMBED_N_CTX=512
    )
fi

env "${KVBFS_ENV[@]}" "$KVBFS_BIN" "$MOUNT" -f > /tmp/kvbfs_e2e.log 2>&1 &
KVBFS_PID=$!

# Wait for FUSE mount to be ready (model loading may take a while)
echo "  Waiting for mount (model loading may take ~30s)..."
for i in $(seq 1 120); do
    if [ -d "$MOUNT/sessions" ] 2>/dev/null; then
        echo "  Mount ready after ${i}s"
        break
    fi
    if ! kill -0 "$KVBFS_PID" 2>/dev/null; then
        echo -e "  ${RED}kvbfs process died during startup${NC}"
        cat /tmp/kvbfs_e2e.log | tail -20
        exit 1
    fi
    sleep 1
done
check "kvbfs 进程存活" "kill -0 $KVBFS_PID 2>/dev/null"
check "/sessions 目录存在" "[ -d '$MOUNT/sessions' ]"

# ── 测试 1: 单轮对话 ─────────────────────────────────

echo -e "\n${CYAN}[2] 单轮对话${NC}"
echo "User: What is 2+3?" > "$MOUNT/sessions/math"
check "等待回复" "wait_response '$MOUNT/sessions/math' 1"
check "回复包含 5" "grep -q '5' '$MOUNT/sessions/math'"
echo "---"
cat "$MOUNT/sessions/math"
echo "---"

# ── 测试 2: 多轮对话 ─────────────────────────────────

echo -e "\n${CYAN}[3] 多轮对话${NC}"
echo "User: My name is Alice." > "$MOUNT/sessions/multi"
wait_response "$MOUNT/sessions/multi" 1

echo "User: What is my name?" >> "$MOUNT/sessions/multi"
check "等待第二轮回复" "wait_response '$MOUNT/sessions/multi' 2"
check "回复记住名字" "grep -qi 'alice' '$MOUNT/sessions/multi'"
echo "---"
cat "$MOUNT/sessions/multi"
echo "---"

# ── 测试 3: 中文对话 ─────────────────────────────────

echo -e "\n${CYAN}[4] 中文对话${NC}"
echo "User: 请用一句话介绍地球" > "$MOUNT/sessions/chinese"
check "等待中文回复" "wait_response '$MOUNT/sessions/chinese' 1"
echo "---"
cat "$MOUNT/sessions/chinese"
echo "---"

# ── 测试 4: Session 隔离 ─────────────────────────────

echo -e "\n${CYAN}[5] Session 隔离${NC}"
math_lines=$(wc -l < "$MOUNT/sessions/math")
multi_lines=$(wc -l < "$MOUNT/sessions/multi")
check "math 文件未被其他 session 修改" "[ '$math_lines' -le 3 ]"
check "multi 文件行数 > math" "[ '$multi_lines' -gt '$math_lines' ]"

# ── 测试 5: 只读不触发 ──────────────────────────────

echo -e "\n${CYAN}[6] 读操作不触发推理${NC}"
before=$(wc -l < "$MOUNT/sessions/math")
cat "$MOUNT/sessions/math" > /dev/null
sleep 2
after=$(wc -l < "$MOUNT/sessions/math")
check "cat 未触发新回复" "[ '$before' -eq '$after' ]"

# ── 记忆测试 (仅当 MEM_ENABLED=1) ────────────────────

if [ "$MEM_ENABLED" -eq 1 ]; then

    # ── 测试 A: Embedding 存储与语义搜索 ─────────────
    echo -e "\n${CYAN}[7] Embedding 存储与语义搜索${NC}"

    # 写入多主题对话
    echo "User: Write a Python function to sort a list" > "$MOUNT/sessions/mem_prog"
    wait_response "$MOUNT/sessions/mem_prog" 1

    echo "User: What is the weather like in Tokyo?" > "$MOUNT/sessions/mem_weather"
    wait_response "$MOUNT/sessions/mem_weather" 1

    # 等待 embedding worker 处理完 (异步)
    echo "  等待 embedding 处理..."
    sleep 5

    # 通过 ioctl 查询编程相关记忆
    echo "  查询: 'programming code function'..."
    "$MEM_IOCTL_BIN" "$MOUNT/sessions/mem_prog" "programming code function" 5 > /tmp/kvbfs_mem_search.out 2>&1
    SEARCH_RC=$?
    echo "  搜索结果:"
    head -5 /tmp/kvbfs_mem_search.out | sed 's/^/    /'

    check "ioctl 搜索返回结果" "[ $SEARCH_RC -eq 0 ]"
    # 验证第一行有分数
    FIRST_SCORE=$(head -1 /tmp/kvbfs_mem_search.out | cut -f1)
    check "搜索分数非空" "[ -n '$FIRST_SCORE' ]"

    # ── 测试 B: 跨 Session 记忆搜索 ─────────────────
    echo -e "\n${CYAN}[8] 跨 Session 记忆搜索${NC}"

    # 从 weather session 搜索 programming 相关内容（应该能跨 session 找到）
    "$MEM_IOCTL_BIN" "$MOUNT/sessions/mem_weather" "Python sort algorithm" 5 > /tmp/kvbfs_mem_cross.out 2>&1
    CROSS_RC=$?
    echo "  跨 Session 搜索结果:"
    head -3 /tmp/kvbfs_mem_cross.out | sed 's/^/    /'

    check "跨 Session 搜索返回结果" "[ $CROSS_RC -eq 0 ]"

    # ── 测试 C: 上下文压缩 ──────────────────────────
    echo -e "\n${CYAN}[9] 上下文压缩${NC}"

    # 创建一个较长的对话来触发压缩
    # n_ctx=2048, 阈值=75%=1536 tokens, 每条消息约几十token
    # 需要足够多轮对话来接近阈值
    echo "User: Tell me about the history of computing." > "$MOUNT/sessions/mem_compress"
    wait_response "$MOUNT/sessions/mem_compress" 1

    echo "User: Who invented the first computer?" >> "$MOUNT/sessions/mem_compress"
    wait_response "$MOUNT/sessions/mem_compress" 2

    echo "User: What was ENIAC and when was it built?" >> "$MOUNT/sessions/mem_compress"
    wait_response "$MOUNT/sessions/mem_compress" 3

    echo "User: Tell me about Alan Turing and his contributions to computing." >> "$MOUNT/sessions/mem_compress"
    wait_response "$MOUNT/sessions/mem_compress" 4

    echo "User: How did programming languages evolve from machine code to high-level languages?" >> "$MOUNT/sessions/mem_compress"
    wait_response "$MOUNT/sessions/mem_compress" 5

    echo "User: What was the significance of the transistor for computing?" >> "$MOUNT/sessions/mem_compress"
    wait_response "$MOUNT/sessions/mem_compress" 6

    echo "User: Describe the evolution from mainframes to personal computers." >> "$MOUNT/sessions/mem_compress"
    wait_response "$MOUNT/sessions/mem_compress" 7

    echo "User: How did the internet change computing forever?" >> "$MOUNT/sessions/mem_compress"
    wait_response "$MOUNT/sessions/mem_compress" 8

    echo "User: What are the most important computing innovations of the 21st century?" >> "$MOUNT/sessions/mem_compress"
    wait_response "$MOUNT/sessions/mem_compress" 9

    echo "User: Summarize all the topics we discussed about computing history." >> "$MOUNT/sessions/mem_compress"
    wait_response "$MOUNT/sessions/mem_compress" 10

    # 检查是否触发了上下文压缩
    if grep -q "\[Context Summary\]" "$MOUNT/sessions/mem_compress" 2>/dev/null; then
        check "上下文压缩已触发" "true"
        echo "  ---"
        head -5 "$MOUNT/sessions/mem_compress"
        echo "  ..."
        echo "  ---"
    else
        echo "  注意: 上下文压缩未触发 (对话可能未超过 75% token 阈值)"
        check "对话仍然完整" "grep -c '^Assistant:' '$MOUNT/sessions/mem_compress' | grep -q '[5-9]\|1[0-9]'"
    fi

    # 如果压缩触发了，验证压缩后仍能继续对话
    if grep -q "\[Context Summary\]" "$MOUNT/sessions/mem_compress" 2>/dev/null; then
        echo "User: Can you still answer questions after compression?" >> "$MOUNT/sessions/mem_compress"
        prev_count=$(grep -c "^Assistant:" "$MOUNT/sessions/mem_compress" || true)
        next_count=$((prev_count + 1))
        check "压缩后仍能对话" "wait_response '$MOUNT/sessions/mem_compress' $next_count"
    fi

fi

# ── 结果 ──────────────────────────────────────────────

echo -e "\n${CYAN}=== 结果: ${GREEN}${pass} passed${NC}, ${RED}${fail} failed${NC} ==="
[ "$fail" -eq 0 ] && exit 0 || exit 1
