#!/bin/bash
#
# kvbfs filesystem integration test
#
# Usage: ./test_kvbfs.sh [mountpoint] [kvbfs_binary]
#

set -uo pipefail

MNT="${1:-/tmp/kvbfs_mnt}"
KVBFS="${2:-$HOME/kvbfs-build/kvbfs}"
DB_PATH="/tmp/kvbfs_test_data_$$"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASS=0
FAIL=0
SKIP=0

pass() { ((PASS++)); echo -e "  ${GREEN}PASS${NC}: $1"; }
fail() { ((FAIL++)); echo -e "  ${RED}FAIL${NC}: $1${2:+ ($2)}"; }
skip() { ((SKIP++)); echo -e "  ${YELLOW}SKIP${NC}: $1"; }

cleanup() {
    echo ""
    echo "=== Cleanup ==="
    fusermount3 -u "$MNT" 2>/dev/null || true
    sleep 1
    if [ -n "${KVBFS_PID:-}" ] && kill -0 "$KVBFS_PID" 2>/dev/null; then
        kill "$KVBFS_PID" 2>/dev/null || true
        wait "$KVBFS_PID" 2>/dev/null || true
    fi
    rm -rf "$DB_PATH"
    echo "Done."
}
trap cleanup EXIT

# Clean up any stale mount from previous run
fusermount3 -u "$MNT" 2>/dev/null || true
rm -rf "$MNT"
mkdir -p "$MNT"

# ============================================================
echo "=== kvbfs Integration Test ==="
echo "  Binary:     $KVBFS"
echo "  Mountpoint: $MNT"
echo "  DB path:    $DB_PATH"
echo ""

if [ ! -x "$KVBFS" ]; then
    echo "ERROR: kvbfs binary not found at $KVBFS"
    exit 1
fi

# Prepare
rm -rf "$DB_PATH"

# Start kvbfs in foreground background (single-threaded to avoid max_idle_threads warning)
export KVBFS_DB_PATH="$DB_PATH"
"$KVBFS" "$MNT" -f -s &
KVBFS_PID=$!

# Wait for mount to become ready
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

echo "kvbfs started (PID=$KVBFS_PID)"
echo ""

# ============================================================
echo "--- Test 1: Root directory listing (readdir, getattr) ---"
if ls "$MNT/" >/dev/null 2>&1; then
    pass "ls on root directory"
else
    fail "ls on root directory"
fi

# ============================================================
echo "--- Test 2: Create file (create, write, release) ---"
if echo "hello kvbfs" > "$MNT/test.txt" 2>/dev/null; then
    pass "create and write file"
else
    fail "create and write file"
fi

# ============================================================
echo "--- Test 3: Read file (open, read, release) ---"
CONTENT=$(cat "$MNT/test.txt" 2>/dev/null || echo "READ_ERROR")
if [ "$CONTENT" = "hello kvbfs" ]; then
    pass "read file content matches"
else
    fail "read file content" "expected 'hello kvbfs', got '$CONTENT'"
fi

# ============================================================
echo "--- Test 4: File in directory listing (readdir, lookup) ---"
if ls "$MNT/" 2>/dev/null | grep -q "test.txt"; then
    pass "file appears in directory listing"
else
    fail "file appears in directory listing"
fi

# ============================================================
echo "--- Test 5: File stat (getattr) ---"
if stat "$MNT/test.txt" >/dev/null 2>&1; then
    SIZE=$(stat -c %s "$MNT/test.txt" 2>/dev/null || echo 0)
    if [ "$SIZE" -gt 0 ]; then
        pass "stat file (size=$SIZE)"
    else
        fail "stat file" "size is 0"
    fi
else
    fail "stat file"
fi

# ============================================================
echo "--- Test 6: Create directory (mkdir) ---"
if mkdir "$MNT/subdir" 2>/dev/null; then
    pass "mkdir"
else
    fail "mkdir"
fi

# ============================================================
echo "--- Test 7: Directory stat ---"
if stat "$MNT/subdir" >/dev/null 2>&1; then
    FTYPE=$(stat -c %F "$MNT/subdir" 2>/dev/null || echo "unknown")
    if [ "$FTYPE" = "directory" ]; then
        pass "directory type correct"
    else
        fail "directory type" "got '$FTYPE'"
    fi
else
    fail "stat directory"
fi

# ============================================================
echo "--- Test 8: Create file in subdirectory ---"
if echo "nested file" > "$MNT/subdir/nested.txt" 2>/dev/null; then
    pass "create file in subdirectory"
else
    fail "create file in subdirectory"
fi

CONTENT=$(cat "$MNT/subdir/nested.txt" 2>/dev/null || echo "READ_ERROR")
if [ "$CONTENT" = "nested file" ]; then
    pass "read nested file"
else
    fail "read nested file" "expected 'nested file', got '$CONTENT'"
fi

# ============================================================
echo "--- Test 9: Overwrite file (write) ---"
if echo "overwritten" > "$MNT/test.txt" 2>/dev/null; then
    CONTENT=$(cat "$MNT/test.txt" 2>/dev/null || echo "READ_ERROR")
    if [ "$CONTENT" = "overwritten" ]; then
        pass "overwrite file"
    else
        fail "overwrite file" "got '$CONTENT'"
    fi
else
    fail "overwrite file"
fi

# ============================================================
echo "--- Test 10: Append to file ---"
if echo "line2" >> "$MNT/test.txt" 2>/dev/null; then
    LINES=$(wc -l < "$MNT/test.txt" 2>/dev/null || echo 0)
    if [ "$LINES" -ge 2 ]; then
        pass "append to file (lines=$LINES)"
    else
        fail "append to file" "expected >= 2 lines, got $LINES"
    fi
else
    fail "append to file"
fi

# ============================================================
echo "--- Test 11: Large file write/read ---"
dd if=/dev/urandom bs=1024 count=256 2>/dev/null | base64 > "$MNT/large.bin" 2>/dev/null
ORIG_SUM=$(md5sum "$MNT/large.bin" 2>/dev/null | awk '{print $1}')
# Copy out and compare
cp "$MNT/large.bin" /tmp/kvbfs_test_copy_$$ 2>/dev/null
COPY_SUM=$(md5sum /tmp/kvbfs_test_copy_$$ 2>/dev/null | awk '{print $1}')
rm -f /tmp/kvbfs_test_copy_$$
if [ -n "$ORIG_SUM" ] && [ "$ORIG_SUM" = "$COPY_SUM" ]; then
    SIZE=$(stat -c %s "$MNT/large.bin" 2>/dev/null || echo 0)
    pass "large file integrity (size=$SIZE)"
else
    fail "large file integrity" "checksums differ"
fi

# ============================================================
echo "--- Test 12: Rename file (rename) ---"
if mv "$MNT/test.txt" "$MNT/renamed.txt" 2>/dev/null; then
    if [ -f "$MNT/renamed.txt" ] && [ ! -f "$MNT/test.txt" ]; then
        pass "rename file"
    else
        fail "rename file" "old file still exists or new file missing"
    fi
else
    fail "rename file"
fi

# ============================================================
echo "--- Test 13: Rename across directories ---"
if mv "$MNT/renamed.txt" "$MNT/subdir/moved.txt" 2>/dev/null; then
    if [ -f "$MNT/subdir/moved.txt" ] && [ ! -f "$MNT/renamed.txt" ]; then
        pass "rename across directories"
    else
        fail "rename across directories"
    fi
else
    fail "rename across directories"
fi

# ============================================================
echo "--- Test 14: Remove file (unlink) ---"
if rm "$MNT/subdir/moved.txt" 2>/dev/null; then
    if [ ! -f "$MNT/subdir/moved.txt" ]; then
        pass "unlink file"
    else
        fail "unlink file" "file still exists"
    fi
else
    fail "unlink file"
fi

# ============================================================
echo "--- Test 15: Remove file in subdirectory ---"
if rm "$MNT/subdir/nested.txt" 2>/dev/null; then
    pass "unlink nested file"
else
    fail "unlink nested file"
fi

# ============================================================
echo "--- Test 16: Remove directory (rmdir) ---"
if rmdir "$MNT/subdir" 2>/dev/null; then
    if [ ! -d "$MNT/subdir" ]; then
        pass "rmdir"
    else
        fail "rmdir" "directory still exists"
    fi
else
    fail "rmdir"
fi

# ============================================================
echo "--- Test 17: rmdir on non-empty directory should fail ---"
mkdir "$MNT/notempty" 2>/dev/null
echo "data" > "$MNT/notempty/file.txt" 2>/dev/null
if rmdir "$MNT/notempty" 2>/dev/null; then
    fail "rmdir non-empty" "should have failed"
else
    pass "rmdir non-empty correctly rejected"
fi
rm -f "$MNT/notempty/file.txt" 2>/dev/null
rmdir "$MNT/notempty" 2>/dev/null

# ============================================================
echo "--- Test 18: chmod (setattr) ---"
echo "perm test" > "$MNT/permfile.txt" 2>/dev/null
if chmod 755 "$MNT/permfile.txt" 2>/dev/null; then
    PERM=$(stat -c %a "$MNT/permfile.txt" 2>/dev/null || echo "000")
    if [ "$PERM" = "755" ]; then
        pass "chmod"
    else
        fail "chmod" "expected 755, got $PERM"
    fi
else
    fail "chmod"
fi

# ============================================================
echo "--- Test 19: truncate (setattr) ---"
echo "truncate me please" > "$MNT/trunc.txt" 2>/dev/null
if truncate -s 5 "$MNT/trunc.txt" 2>/dev/null; then
    SIZE=$(stat -c %s "$MNT/trunc.txt" 2>/dev/null || echo 0)
    if [ "$SIZE" = "5" ]; then
        pass "truncate (size=$SIZE)"
    else
        fail "truncate" "expected 5, got $SIZE"
    fi
else
    fail "truncate"
fi

# ============================================================
echo "--- Test 20: fsync ---"
echo "fsync test" > "$MNT/syncfile.txt" 2>/dev/null
# Use dd with conv=fsync to trigger fsync
if dd if=/dev/zero of="$MNT/syncfile.txt" bs=64 count=1 conv=fsync 2>/dev/null; then
    pass "fsync"
else
    fail "fsync"
fi

# ============================================================
echo "--- Test 21: Multiple files ---"
for i in $(seq 1 20); do
    echo "file number $i" > "$MNT/multi_$i.txt" 2>/dev/null
done
COUNT=$(ls "$MNT"/multi_*.txt 2>/dev/null | wc -l)
if [ "$COUNT" -eq 20 ]; then
    pass "create 20 files"
else
    fail "create 20 files" "found $COUNT"
fi

# Verify contents of a few
OK=true
for i in 1 10 20; do
    C=$(cat "$MNT/multi_$i.txt" 2>/dev/null)
    if [ "$C" != "file number $i" ]; then
        OK=false
        break
    fi
done
if $OK; then
    pass "verify multiple file contents"
else
    fail "verify multiple file contents"
fi

# Cleanup test files
rm -f "$MNT"/multi_*.txt "$MNT/permfile.txt" "$MNT/trunc.txt" "$MNT/syncfile.txt" "$MNT/large.bin" 2>/dev/null

# ============================================================
echo "--- Test 22: Create and read symlink ---"
echo "symlink_target_data" > "$MNT/sym_target.txt" 2>/dev/null
if ln -s sym_target.txt "$MNT/sym_link.txt" 2>/dev/null; then
    LINK=$(readlink "$MNT/sym_link.txt" 2>/dev/null || echo "READLINK_ERROR")
    if [ "$LINK" = "sym_target.txt" ]; then
        pass "create and readlink symlink"
    else
        fail "readlink symlink" "expected 'sym_target.txt', got '$LINK'"
    fi
else
    fail "create symlink"
fi

# ============================================================
echo "--- Test 23: Access through symlink ---"
CONTENT=$(cat "$MNT/sym_link.txt" 2>/dev/null || echo "READ_ERROR")
if [ "$CONTENT" = "symlink_target_data" ]; then
    pass "read through symlink"
else
    fail "read through symlink" "expected 'symlink_target_data', got '$CONTENT'"
fi

# ============================================================
echo "--- Test 24: Dangling symlink ---"
if ln -s nonexistent_file "$MNT/dangling_link" 2>/dev/null; then
    LINK=$(readlink "$MNT/dangling_link" 2>/dev/null || echo "READLINK_ERROR")
    if [ "$LINK" = "nonexistent_file" ]; then
        pass "dangling symlink readlink"
    else
        fail "dangling symlink readlink" "got '$LINK'"
    fi
else
    fail "create dangling symlink"
fi

# ============================================================
echo "--- Test 25: Remove symlink ---"
if rm "$MNT/sym_link.txt" 2>/dev/null; then
    if [ ! -L "$MNT/sym_link.txt" ] && [ -f "$MNT/sym_target.txt" ]; then
        pass "remove symlink (target intact)"
    else
        fail "remove symlink" "symlink still exists or target removed"
    fi
else
    fail "remove symlink"
fi

# ============================================================
echo "--- Test 26: Create hardlink ---"
echo "hardlink_data" > "$MNT/hl_orig.txt" 2>/dev/null
if ln "$MNT/hl_orig.txt" "$MNT/hl_link.txt" 2>/dev/null; then
    ORIG_INO=$(stat -c %i "$MNT/hl_orig.txt" 2>/dev/null || echo "0")
    LINK_INO=$(stat -c %i "$MNT/hl_link.txt" 2>/dev/null || echo "1")
    if [ "$ORIG_INO" = "$LINK_INO" ]; then
        pass "hardlink same inode"
    else
        fail "hardlink same inode" "orig=$ORIG_INO link=$LINK_INO"
    fi
else
    fail "create hardlink"
fi

# ============================================================
echo "--- Test 27: Hardlink nlink count ---"
NLINK=$(stat -c %h "$MNT/hl_orig.txt" 2>/dev/null || echo "0")
CONTENT=$(cat "$MNT/hl_link.txt" 2>/dev/null || echo "READ_ERROR")
if [ "$NLINK" = "2" ] && [ "$CONTENT" = "hardlink_data" ]; then
    pass "hardlink nlink=2 and content matches"
else
    fail "hardlink nlink/content" "nlink=$NLINK content='$CONTENT'"
fi

# ============================================================
echo "--- Test 28: Remove original, hardlink survives ---"
if rm "$MNT/hl_orig.txt" 2>/dev/null; then
    CONTENT=$(cat "$MNT/hl_link.txt" 2>/dev/null || echo "READ_ERROR")
    if [ "$CONTENT" = "hardlink_data" ]; then
        pass "hardlink survives after original removed"
    else
        fail "hardlink survives" "got '$CONTENT'"
    fi
else
    fail "remove original of hardlink"
fi

# ============================================================
echo "--- Test 29: Symlink in subdirectory ---"
mkdir "$MNT/linkdir" 2>/dev/null
echo "cross_dir_data" > "$MNT/linkdir/real.txt" 2>/dev/null
if ln -s linkdir/real.txt "$MNT/cross_link.txt" 2>/dev/null; then
    CONTENT=$(cat "$MNT/cross_link.txt" 2>/dev/null || echo "READ_ERROR")
    if [ "$CONTENT" = "cross_dir_data" ]; then
        pass "symlink across directories"
    else
        fail "symlink across directories" "got '$CONTENT'"
    fi
else
    fail "create cross-directory symlink"
fi

# Cleanup symlink/hardlink test files
rm -f "$MNT/sym_target.txt" "$MNT/dangling_link" "$MNT/hl_link.txt" "$MNT/cross_link.txt" 2>/dev/null
rm -f "$MNT/linkdir/real.txt" 2>/dev/null
rmdir "$MNT/linkdir" 2>/dev/null

# ============================================================
echo ""
echo "========================================="
echo -e "  ${GREEN}PASS: $PASS${NC}  ${RED}FAIL: $FAIL${NC}  ${YELLOW}SKIP: $SKIP${NC}"
echo "========================================="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
