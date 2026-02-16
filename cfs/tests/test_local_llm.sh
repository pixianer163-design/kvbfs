#!/bin/bash
# CFS 本地模型端到端测试
# 用法: ./test_local_llm.sh [model_path]

set -e

MODEL=${1:-/mnt/d/project/kvbfs/cfs/models/qwen2.5-0.5b-instruct-q4_k_m.gguf}
KVBFS_BIN="$(dirname "$0")/../../build/kvbfs"
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
        ((pass++))
    else
        echo -e "  ${RED}FAIL${NC}: $1"
        ((fail++))
    fi
}

wait_response() {
    local file=$1
    local expect_count=${2:-1}
    for i in $(seq 1 90); do
        local count=$(grep -c "^Assistant:" "$file" 2>/dev/null || echo 0)
        if [ "$count" -ge "$expect_count" ]; then
            return 0
        fi
        sleep 1
    done
    return 1
}

cleanup() {
    echo -e "\n${CYAN}清理...${NC}"
    kill "$KVBFS_PID" 2>/dev/null && wait "$KVBFS_PID" 2>/dev/null
    fusermount3 -u "$MOUNT" 2>/dev/null || fusermount -u "$MOUNT" 2>/dev/null || true
    rm -rf "$MOUNT" "$DATA"
}
trap cleanup EXIT

# ── 准备 ──────────────────────────────────────────────

echo -e "${CYAN}=== CFS 本地 LLM 端到端测试 ===${NC}"
echo "模型: $MODEL"
echo "二进制: $KVBFS_BIN"

if [ ! -f "$MODEL" ]; then
    echo -e "${RED}错误: 模型文件不存在: $MODEL${NC}"
    exit 1
fi

if [ ! -x "$KVBFS_BIN" ]; then
    echo -e "${RED}错误: kvbfs 未编译或不可执行: $KVBFS_BIN${NC}"
    exit 1
fi

rm -rf "$MOUNT" "$DATA"
mkdir -p "$MOUNT" "$DATA"

# ── 启动 kvbfs ────────────────────────────────────────

echo -e "\n${CYAN}[1] 启动 kvbfs + 本地 LLM${NC}"
KVBFS_DB_PATH="$DATA" \
CFS_MODEL_PATH="$MODEL" \
CFS_N_CTX=2048 \
CFS_MAX_TOKENS=128 \
CFS_TEMPERATURE=0.3 \
    "$KVBFS_BIN" "$MOUNT" -f > /tmp/kvbfs_e2e.log 2>&1 &
KVBFS_PID=$!

sleep 3
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

# ── 结果 ──────────────────────────────────────────────

echo -e "\n${CYAN}=== 结果: ${GREEN}${pass} passed${NC}, ${RED}${fail} failed${NC} ==="
[ "$fail" -eq 0 ] && exit 0 || exit 1
