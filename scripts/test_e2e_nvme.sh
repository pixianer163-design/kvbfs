#!/bin/bash
#
# kvbfs + NVMe KV 模拟器 端到端测试
#
# Usage: ./test_e2e_nvme.sh [build_dir]
#

set -uo pipefail

BUILD_DIR="${1:-build-nvme}"
SIM_BIN="$BUILD_DIR/sim/nvme-kv-sim"
KVBFS_BIN="$BUILD_DIR/kvbfs"
MNT="/tmp/kvbfs_nvme_test_$$"
PORT=9527

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASS=0
FAIL=0

pass() { ((PASS++)); echo -e "  ${GREEN}PASS${NC}: $1"; }
fail() { ((FAIL++)); echo -e "  ${RED}FAIL${NC}: $1${2:+ ($2)}"; }

cleanup() {
    echo ""
    echo "=== Cleanup ==="
    fusermount3 -u "$MNT" 2>/dev/null || true
    sleep 1
    if [ -n "${KVBFS_PID:-}" ] && kill -0 "$KVBFS_PID" 2>/dev/null; then
        kill "$KVBFS_PID" 2>/dev/null || true
        wait "$KVBFS_PID" 2>/dev/null || true
    fi
    if [ -n "${SIM_PID:-}" ] && kill -0 "$SIM_PID" 2>/dev/null; then
        kill "$SIM_PID" 2>/dev/null || true
        wait "$SIM_PID" 2>/dev/null || true
    fi
    rm -rf "$MNT"
    echo "Done."
}
trap cleanup EXIT

# ============================================================
echo "=== kvbfs + NVMe KV Simulator E2E Test ==="
echo "  Simulator: $SIM_BIN"
echo "  KVBFS:     $KVBFS_BIN"
echo "  Mount:     $MNT"
echo "  Port:      $PORT"
echo ""

# 检查二进制文件
for bin in "$SIM_BIN" "$KVBFS_BIN"; do
    if [ ! -x "$bin" ]; then
        echo "ERROR: binary not found: $bin"
        echo "Build with: cmake -B $BUILD_DIR -DKVBFS_BACKEND=nvme && cmake --build $BUILD_DIR"
        exit 1
    fi
done

# 清理旧挂载
fusermount3 -u "$MNT" 2>/dev/null || true
rm -rf "$MNT"
mkdir -p "$MNT"

# 启动模拟器
"$SIM_BIN" --port "$PORT" &
SIM_PID=$!
sleep 1

if ! kill -0 "$SIM_PID" 2>/dev/null; then
    echo "ERROR: simulator failed to start"
    exit 1
fi
echo "Simulator started (PID=$SIM_PID)"

# 启动 kvbfs
export KVBFS_DB_PATH="127.0.0.1:$PORT"
"$KVBFS_BIN" "$MNT" -f -s &
KVBFS_PID=$!

# 等待挂载就绪
RETRIES=0
while ! mountpoint -q "$MNT" 2>/dev/null; do
    if ! kill -0 "$KVBFS_PID" 2>/dev/null; then
        echo "ERROR: kvbfs failed to start"
        exit 1
    fi
    sleep 1
    RETRIES=$((RETRIES + 1))
    if [ "$RETRIES" -ge 10 ]; then
        echo "ERROR: kvbfs mount timed out"
        exit 1
    fi
done
echo "KVBFS mounted (PID=$KVBFS_PID)"
echo ""

# ============================================================
echo "--- Test 1: Root directory listing ---"
if ls "$MNT/" >/dev/null 2>&1; then
    pass "ls root"
else
    fail "ls root"
fi

# ============================================================
echo "--- Test 2: Create and read file ---"
if echo "hello nvme" > "$MNT/test.txt" 2>/dev/null; then
    pass "create file"
else
    fail "create file"
fi

CONTENT=$(cat "$MNT/test.txt" 2>/dev/null || echo "READ_ERROR")
if [ "$CONTENT" = "hello nvme" ]; then
    pass "read file"
else
    fail "read file" "got '$CONTENT'"
fi

# ============================================================
echo "--- Test 3: File in listing ---"
if ls "$MNT/" 2>/dev/null | grep -q "test.txt"; then
    pass "file in listing"
else
    fail "file in listing"
fi

# ============================================================
echo "--- Test 4: Create directory ---"
if mkdir "$MNT/subdir" 2>/dev/null; then
    pass "mkdir"
else
    fail "mkdir"
fi

# ============================================================
echo "--- Test 5: File in subdirectory ---"
if echo "nested" > "$MNT/subdir/nested.txt" 2>/dev/null; then
    CONTENT=$(cat "$MNT/subdir/nested.txt" 2>/dev/null || echo "READ_ERROR")
    if [ "$CONTENT" = "nested" ]; then
        pass "nested file"
    else
        fail "nested file" "got '$CONTENT'"
    fi
else
    fail "create nested file"
fi

# ============================================================
echo "--- Test 6: Overwrite file ---"
if echo "updated" > "$MNT/test.txt" 2>/dev/null; then
    CONTENT=$(cat "$MNT/test.txt" 2>/dev/null || echo "READ_ERROR")
    if [ "$CONTENT" = "updated" ]; then
        pass "overwrite"
    else
        fail "overwrite" "got '$CONTENT'"
    fi
else
    fail "overwrite"
fi

# ============================================================
echo "--- Test 7: Remove file ---"
if rm "$MNT/subdir/nested.txt" 2>/dev/null; then
    if [ ! -f "$MNT/subdir/nested.txt" ]; then
        pass "unlink"
    else
        fail "unlink" "file still exists"
    fi
else
    fail "unlink"
fi

# ============================================================
echo "--- Test 8: Remove directory ---"
if rmdir "$MNT/subdir" 2>/dev/null; then
    pass "rmdir"
else
    fail "rmdir"
fi

# ============================================================
echo "--- Test 9: Multiple files ---"
for i in $(seq 1 10); do
    echo "file $i" > "$MNT/multi_$i.txt" 2>/dev/null
done
COUNT=$(ls "$MNT"/multi_*.txt 2>/dev/null | wc -l)
if [ "$COUNT" -eq 10 ]; then
    pass "create 10 files"
else
    fail "create 10 files" "found $COUNT"
fi

# ============================================================
echo "--- Test 10: Symlink create and readlink ---"
echo "nvme_sym_data" > "$MNT/sym_target.txt" 2>/dev/null
if ln -s sym_target.txt "$MNT/sym_link.txt" 2>/dev/null; then
    LINK=$(readlink "$MNT/sym_link.txt" 2>/dev/null || echo "READLINK_ERROR")
    CONTENT=$(cat "$MNT/sym_link.txt" 2>/dev/null || echo "READ_ERROR")
    if [ "$LINK" = "sym_target.txt" ] && [ "$CONTENT" = "nvme_sym_data" ]; then
        pass "symlink"
    else
        fail "symlink" "readlink='$LINK' content='$CONTENT'"
    fi
else
    fail "create symlink"
fi

# ============================================================
echo "--- Test 11: Hardlink ---"
echo "nvme_hl_data" > "$MNT/hl_orig.txt" 2>/dev/null
if ln "$MNT/hl_orig.txt" "$MNT/hl_link.txt" 2>/dev/null; then
    CONTENT=$(cat "$MNT/hl_link.txt" 2>/dev/null || echo "READ_ERROR")
    NLINK=$(stat -c %h "$MNT/hl_orig.txt" 2>/dev/null || echo "0")
    if [ "$CONTENT" = "nvme_hl_data" ] && [ "$NLINK" = "2" ]; then
        pass "hardlink"
    else
        fail "hardlink" "content='$CONTENT' nlink=$NLINK"
    fi
else
    fail "create hardlink"
fi

# Cleanup test files
rm -f "$MNT"/multi_*.txt "$MNT/test.txt" "$MNT/sym_target.txt" "$MNT/sym_link.txt" "$MNT/hl_orig.txt" "$MNT/hl_link.txt" 2>/dev/null

# ============================================================
echo ""
echo "========================================="
echo -e "  ${GREEN}PASS: $PASS${NC}  ${RED}FAIL: $FAIL${NC}"
echo "========================================="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
