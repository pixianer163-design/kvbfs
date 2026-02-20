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
# xattr tests use Python os module (setfattr/getfattr may not be installed)
echo "--- Test 30: Set and get xattr ---"
echo "xattr test" > "$MNT/xa_file.txt" 2>/dev/null
XA_RESULT=$(python3 -c "
import os, sys
f = '$MNT/xa_file.txt'
try:
    os.setxattr(f, 'user.status', b'done')
    v = os.getxattr(f, 'user.status')
    if v == b'done':
        print('PASS')
    else:
        print('FAIL:got ' + repr(v))
except Exception as e:
    print('FAIL:' + str(e))
" 2>&1)
if [ "$XA_RESULT" = "PASS" ]; then
    pass "set and get xattr"
else
    fail "set and get xattr" "$XA_RESULT"
fi

# ============================================================
echo "--- Test 31: List xattrs ---"
XA_RESULT=$(python3 -c "
import os
f = '$MNT/xa_file.txt'
try:
    os.setxattr(f, 'user.tag', b'important')
    attrs = os.listxattr(f)
    if 'user.status' in attrs and 'user.tag' in attrs:
        print('PASS')
    else:
        print('FAIL:' + str(attrs))
except Exception as e:
    print('FAIL:' + str(e))
" 2>&1)
if [ "$XA_RESULT" = "PASS" ]; then
    pass "list xattrs"
else
    fail "list xattrs" "$XA_RESULT"
fi

# ============================================================
echo "--- Test 32: Remove xattr ---"
XA_RESULT=$(python3 -c "
import os
f = '$MNT/xa_file.txt'
try:
    os.removexattr(f, 'user.tag')
    attrs = os.listxattr(f)
    if 'user.tag' not in attrs and 'user.status' in attrs:
        print('PASS')
    else:
        print('FAIL:still in list ' + str(attrs))
except Exception as e:
    print('FAIL:' + str(e))
" 2>&1)
if [ "$XA_RESULT" = "PASS" ]; then
    pass "remove xattr"
else
    fail "remove xattr" "$XA_RESULT"
fi

# ============================================================
echo "--- Test 33: Get nonexistent xattr fails ---"
XA_RESULT=$(python3 -c "
import os
f = '$MNT/xa_file.txt'
try:
    os.getxattr(f, 'user.nonexistent')
    print('FAIL:should have raised')
except OSError:
    print('PASS')
except Exception as e:
    print('FAIL:' + str(e))
" 2>&1)
if [ "$XA_RESULT" = "PASS" ]; then
    pass "get nonexistent xattr correctly returns error"
else
    fail "get nonexistent xattr" "$XA_RESULT"
fi

# ============================================================
echo "--- Test 34: Xattr survives file read ---"
XA_RESULT=$(python3 -c "
import os
f = '$MNT/xa_file.txt'
try:
    v1 = os.getxattr(f, 'user.status')
    with open(f) as fh: fh.read()  # trigger read
    v2 = os.getxattr(f, 'user.status')
    if v1 == v2:
        print('PASS')
    else:
        print('FAIL:before=' + repr(v1) + ' after=' + repr(v2))
except Exception as e:
    print('FAIL:' + str(e))
" 2>&1)
if [ "$XA_RESULT" = "PASS" ]; then
    pass "xattr persists across reads"
else
    fail "xattr persistence" "$XA_RESULT"
fi

# ============================================================
echo "--- Test 35: Xattr cleaned up on file delete ---"
rm -f "$MNT/xa_file.txt" 2>/dev/null
echo "new" > "$MNT/xa_new.txt" 2>/dev/null
XA_RESULT=$(python3 -c "
import os
f = '$MNT/xa_new.txt'
try:
    os.getxattr(f, 'user.status')
    print('FAIL:xattr inherited')
except OSError:
    print('PASS')
except Exception as e:
    print('FAIL:' + str(e))
" 2>&1)
if [ "$XA_RESULT" = "PASS" ]; then
    pass "xattr not inherited by new files"
else
    fail "xattr inherited by new file" "$XA_RESULT"
fi
rm -f "$MNT/xa_new.txt" 2>/dev/null

# ============================================================
echo "--- Test 36: Version snapshot on write ---"
echo "version1" > "$MNT/ver_file.txt" 2>/dev/null
VER_RESULT=$(python3 -c "
import os, json
f = '$MNT/ver_file.txt'
try:
    ver = os.getxattr(f, 'agentfs.version').decode()
    if int(ver) >= 1:
        print('PASS:' + ver)
    else:
        print('FAIL:ver=' + ver)
except Exception as e:
    print('FAIL:' + str(e))
" 2>&1)
if echo "$VER_RESULT" | grep -q "^PASS"; then
    pass "version snapshot created (${VER_RESULT#PASS:})"
else
    fail "version snapshot" "$VER_RESULT"
fi

# ============================================================
echo "--- Test 37: Version increments on subsequent writes ---"
echo "version2" > "$MNT/ver_file.txt" 2>/dev/null
echo "version3" > "$MNT/ver_file.txt" 2>/dev/null
VER_RESULT=$(python3 -c "
import os
f = '$MNT/ver_file.txt'
try:
    ver = int(os.getxattr(f, 'agentfs.version').decode())
    if ver >= 3:
        print('PASS:' + str(ver))
    else:
        print('FAIL:ver=' + str(ver))
except Exception as e:
    print('FAIL:' + str(e))
" 2>&1)
if echo "$VER_RESULT" | grep -q "^PASS"; then
    pass "version increments (${VER_RESULT#PASS:})"
else
    fail "version increments" "$VER_RESULT"
fi

# ============================================================
echo "--- Test 38: Version list contains metadata ---"
VER_RESULT=$(python3 -c "
import os, json
f = '$MNT/ver_file.txt'
try:
    raw = os.getxattr(f, 'agentfs.versions').decode()
    versions = json.loads(raw)
    if len(versions) >= 2:
        v = versions[-1]
        if 'ver' in v and 'size' in v and 'mtime' in v:
            print('PASS:count=' + str(len(versions)))
        else:
            print('FAIL:missing fields ' + str(v))
    else:
        print('FAIL:too few versions ' + str(len(versions)))
except Exception as e:
    print('FAIL:' + str(e))
" 2>&1)
if echo "$VER_RESULT" | grep -q "^PASS"; then
    pass "version list metadata (${VER_RESULT#PASS:})"
else
    fail "version list metadata" "$VER_RESULT"
fi

# ============================================================
echo "--- Test 39: agentfs.* xattrs are read-only ---"
VER_RESULT=$(python3 -c "
import os
f = '$MNT/ver_file.txt'
try:
    os.setxattr(f, 'agentfs.version', b'999')
    print('FAIL:should have raised')
except OSError as e:
    if e.errno == 1:  # EPERM
        print('PASS')
    else:
        print('FAIL:wrong errno ' + str(e))
except Exception as e:
    print('FAIL:' + str(e))
" 2>&1)
if [ "$VER_RESULT" = "PASS" ]; then
    pass "agentfs.* xattrs are read-only"
else
    fail "agentfs.* read-only" "$VER_RESULT"
fi

# ============================================================
echo "--- Test 40: Version data cleaned up on delete ---"
rm -f "$MNT/ver_file.txt" 2>/dev/null
echo "fresh" > "$MNT/ver_fresh.txt" 2>/dev/null
VER_RESULT=$(python3 -c "
import os, json
f = '$MNT/ver_fresh.txt'
try:
    ver = int(os.getxattr(f, 'agentfs.version').decode())
    raw = os.getxattr(f, 'agentfs.versions').decode()
    versions = json.loads(raw)
    # New file has 1 version (its own write), not 3+ from deleted file
    if ver == 1 and len(versions) == 1:
        print('PASS')
    else:
        print('FAIL:ver=' + str(ver) + ' count=' + str(len(versions)))
except Exception as e:
    print('FAIL:' + str(e))
" 2>&1)
if [ "$VER_RESULT" = "PASS" ]; then
    pass "old versions cleaned up, new file has ver=1"
else
    fail "version cleanup" "$VER_RESULT"
fi
rm -f "$MNT/ver_fresh.txt" 2>/dev/null

# ============================================================
echo "--- Test 41: .agentfs virtual file appears in root listing ---"
LS_ROOT=$(ls -la "$MNT/" 2>/dev/null)
if echo "$LS_ROOT" | grep -q '\.agentfs'; then
    pass ".agentfs appears in root listing"
else
    fail ".agentfs in root listing" "not found in: $(ls "$MNT/")"
fi

# ============================================================
echo "--- Test 42: .agentfs file stat ---"
if [ -f "$MNT/.agentfs" ]; then
    pass ".agentfs is a regular file"
else
    fail ".agentfs stat" "not a regular file or does not exist"
fi

# ============================================================
echo "--- Test 43: .agentfs read without query returns usage ---"
CTL_READ=$(cat "$MNT/.agentfs" 2>/dev/null)
if echo "$CTL_READ" | grep -q '"status"'; then
    pass ".agentfs read returns JSON with status"
else
    fail ".agentfs read" "unexpected: $CTL_READ"
fi

# ============================================================
echo "--- Test 44: .agentfs write+read search cycle ---"
# Write a query then read results (no embeddings loaded, so results should be empty or error)
CTL_RESULT=$(python3 -c "
import os
try:
    fd = os.open('$MNT/.agentfs', os.O_RDWR)
    os.write(fd, b'test query')
    # Seek back to start to read results
    os.lseek(fd, 0, os.SEEK_SET)
    data = os.read(fd, 8192).decode()
    os.close(fd)
    import json
    j = json.loads(data)
    if 'status' in j:
        print('PASS')
    else:
        print('FAIL:no status in response')
except Exception as e:
    print('FAIL:' + str(e))
" 2>&1)
if [ "$CTL_RESULT" = "PASS" ]; then
    pass ".agentfs write+read returns JSON"
else
    fail ".agentfs write+read" "$CTL_RESULT"
fi

# ============================================================
echo "--- Test 45: .agentfs cannot be deleted ---"
rm -f "$MNT/.agentfs" 2>/dev/null
if [ -f "$MNT/.agentfs" ]; then
    pass ".agentfs survives rm attempt"
else
    fail ".agentfs delete protection" "file was deleted"
fi

# ============================================================
echo "--- Test 46: .agentfs xattr operations return ENOTSUP ---"
XATTR_RESULT=$(python3 -c "
import os, errno
try:
    os.setxattr('$MNT/.agentfs', 'user.test', b'val')
    print('FAIL:setxattr succeeded')
except OSError as e:
    if e.errno == errno.ENOTSUP or e.errno == errno.EOPNOTSUPP:
        print('PASS')
    else:
        print('FAIL:wrong errno ' + str(e))
" 2>&1)
if [ "$XATTR_RESULT" = "PASS" ]; then
    pass ".agentfs xattr returns ENOTSUP"
else
    fail ".agentfs xattr" "$XATTR_RESULT"
fi

# ============================================================
echo "--- Test 47: .events virtual file appears in root listing ---"
LS_ROOT=$(ls -la "$MNT/" 2>/dev/null)
if echo "$LS_ROOT" | grep -q '\.events'; then
    pass ".events appears in root listing"
else
    fail ".events in root listing" "not found in: $(ls "$MNT/")"
fi

# ============================================================
echo "--- Test 48: .events file stat ---"
if [ -f "$MNT/.events" ]; then
    pass ".events is a regular file"
else
    fail ".events stat" "not a regular file or does not exist"
fi

# ============================================================
echo "--- Test 49: .events readable after file create ---"
echo "events_test_data" > "$MNT/evt_test.txt" 2>/dev/null
sleep 0.2
EVT_DATA=$(cat "$MNT/.events" 2>/dev/null)
if echo "$EVT_DATA" | grep -q '"create"'; then
    pass ".events contains create event"
else
    fail ".events create event" "got: $EVT_DATA"
fi

# ============================================================
echo "--- Test 50: .events readable after file write ---"
echo "updated" > "$MNT/evt_test.txt" 2>/dev/null
sleep 0.2
EVT_DATA2=$(cat "$MNT/.events" 2>/dev/null)
if echo "$EVT_DATA2" | grep -q '"write"'; then
    pass ".events contains write event"
else
    fail ".events write event" "got: $EVT_DATA2"
fi
rm -f "$MNT/evt_test.txt" 2>/dev/null

# ============================================================
echo "--- Test 51: .events cannot be deleted ---"
rm -f "$MNT/.events" 2>/dev/null
if [ -f "$MNT/.events" ]; then
    pass ".events survives rm attempt"
else
    fail ".events delete protection" "file was deleted"
fi

# ============================================================
echo "--- Test 52: .versions appears in root listing ---"
LS_ROOT=$(ls -la "$MNT/" 2>/dev/null)
if echo "$LS_ROOT" | grep -q '\.versions'; then
    pass ".versions appears in root listing"
else
    fail ".versions in root listing" "not found in: $(ls "$MNT/")"
fi

# ============================================================
echo "--- Test 53: .versions/<file>/ lists version numbers ---"
echo "version one content" > "$MNT/ver_test.txt"
echo "version two content" > "$MNT/ver_test.txt"
sleep 0.3
VER_LIST=$(ls "$MNT/.versions/ver_test.txt/" 2>/dev/null)
if echo "$VER_LIST" | grep -qE '^[0-9]+$|[[:space:]][0-9]+|^[0-9]'; then
    pass ".versions/<file>/ lists version numbers"
else
    fail ".versions version listing" "got: $VER_LIST"
fi

# ============================================================
echo "--- Test 54: .versions/<file>/1 contains v1 content ---"
V1_CONTENT=$(cat "$MNT/.versions/ver_test.txt/1" 2>/dev/null)
if echo "$V1_CONTENT" | grep -q "version one content"; then
    pass ".versions version 1 content correct"
else
    fail ".versions version 1 content" "got: $V1_CONTENT"
fi

# ============================================================
echo "--- Test 55: .versions/<file>/2 contains v2 content ---"
V2_CONTENT=$(cat "$MNT/.versions/ver_test.txt/2" 2>/dev/null)
if echo "$V2_CONTENT" | grep -q "version two content"; then
    pass ".versions version 2 content correct"
else
    fail ".versions version 2 content" "got: $V2_CONTENT"
fi

# ============================================================
echo "--- Test 56: cp from .versions restores old version ---"
cp "$MNT/.versions/ver_test.txt/1" "$MNT/ver_test.txt"
sleep 0.1
RESTORED=$(cat "$MNT/ver_test.txt" 2>/dev/null)
if echo "$RESTORED" | grep -q "version one content"; then
    pass ".versions restore via cp"
else
    fail ".versions restore" "got: $RESTORED"
fi
rm -f "$MNT/ver_test.txt" 2>/dev/null

# ============================================================
echo "--- Test 57: .versions files are read-only ---"
echo "should fail" > "$MNT/.versions/" 2>/dev/null
RET=$?
if [ "$RET" -ne 0 ]; then
    pass ".versions write rejected"
else
    fail ".versions write protection" "write succeeded unexpectedly"
fi

# ============================================================
echo ""
echo "========================================="
echo -e "  ${GREEN}PASS: $PASS${NC}  ${RED}FAIL: $FAIL${NC}  ${YELLOW}SKIP: $SKIP${NC}"
echo "========================================="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
