# Version Virtual Directory Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Expose historical file versions via `/.versions/<path>/<N>` virtual directory tree, enabling POSIX `cat` to read old versions and `cp` to restore them.

**Architecture:** A session-local hash table (`vtree_ctx`) maps virtual inodes to real inodes + version metadata. FUSE `lookup` resolves path components against real KV dirents (for dirs) or `vm:` version keys (for version numbers). All virtual tree operations are read-only. No new compile flags — version snapshots already work unconditionally.

**Tech Stack:** C, FUSE lowlevel, uthash, RocksDB KV (existing), version.h API (existing)

---

### Task 0: Add .gitignore

**Files:**
- Create: `.gitignore`

**Step 1: Write the file**

```
build*/
cfs/.venv/
**/__pycache__/
*.o
*.a
```

**Step 2: Commit**

```bash
git add .gitignore
git commit -m "chore: add .gitignore"
```

---

### Task 1: Create `src/vfs_versions.h`

**Files:**
- Create: `src/vfs_versions.h`

**Step 1: Write the header**

```c
#ifndef VFS_VERSIONS_H
#define VFS_VERSIONS_H

#include <pthread.h>
#include <stdint.h>
#include "uthash.h"

/* Virtual inode constants */
#define AGENTFS_VERSIONS_INO   0xFFFFFFFFFFFFFDULL
#define AGENTFS_VERSIONS_NAME  ".versions"
#define AGENTFS_VDIR_BASE      0xC000000000000001ULL

/* A node in the virtual directory tree.
 * - is_version_file=0: mirrors a real directory or file (version-list dir)
 * - is_version_file=1: a specific version of real_ino, readable via version_read_block
 */
struct vtree_node {
    uint64_t vino;            /* virtual inode number (hash key) */
    uint64_t real_ino;        /* real inode this mirrors */
    int      is_version_file; /* 1=leaf version file, 0=directory */
    uint64_t version;         /* version number (is_version_file=1 only) */
    UT_hash_handle hh;
};

/* Reverse lookup entry: (parent_vino, child_name) → vino */
struct vtree_lookup_entry {
    char     key[80];         /* "%llu:%s" of parent_vino:name */
    uint64_t vino;
    UT_hash_handle hh;
};

/* Per-session virtual tree state */
struct vtree_ctx {
    struct vtree_node         *by_ino;    /* hash by vino */
    struct vtree_lookup_entry *by_parent; /* hash by (parent_vino:name) */
    uint64_t                   next_vino; /* allocation counter */
    pthread_mutex_t            lock;
};

/* Per-open handle for version files (stored in fi->fh) */
struct version_fh {
    uint64_t real_ino;
    uint64_t version;
};

void     vtree_init(struct vtree_ctx *vt);
void     vtree_destroy(struct vtree_ctx *vt);

/* Return vino for (parent_vino, name), or 0 if not cached */
uint64_t vtree_lookup_child(struct vtree_ctx *vt, uint64_t parent_vino,
                            const char *name);

/* Return node for vino, or NULL */
struct vtree_node *vtree_get(struct vtree_ctx *vt, uint64_t vino);

/* Allocate a virtual directory node (idempotent: returns existing if present) */
uint64_t vtree_alloc_dir(struct vtree_ctx *vt, uint64_t parent_vino,
                         const char *name, uint64_t real_ino);

/* Allocate a version file node (idempotent) */
uint64_t vtree_alloc_vfile(struct vtree_ctx *vt, uint64_t parent_vino,
                           const char *name, uint64_t real_ino, uint64_t version);

/* Return 1 if ino belongs to the dynamic virtual tree range */
static inline int vtree_is_vnode(uint64_t ino)
{
    return ino >= AGENTFS_VDIR_BASE && ino < 0xD000000000000000ULL;
}

#endif /* VFS_VERSIONS_H */
```

**Step 2: Commit**

```bash
git add src/vfs_versions.h
git commit -m "feat: add vfs_versions.h virtual tree data structures"
```

---

### Task 2: Create `src/vfs_versions.c`

**Files:**
- Create: `src/vfs_versions.c`

**Step 1: Write the implementation**

```c
#include "vfs_versions.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void vtree_init(struct vtree_ctx *vt)
{
    vt->by_ino    = NULL;
    vt->by_parent = NULL;
    vt->next_vino = AGENTFS_VDIR_BASE;
    pthread_mutex_init(&vt->lock, NULL);
}

void vtree_destroy(struct vtree_ctx *vt)
{
    pthread_mutex_lock(&vt->lock);

    struct vtree_node *n, *ntmp;
    HASH_ITER(hh, vt->by_ino, n, ntmp) {
        HASH_DEL(vt->by_ino, n);
        free(n);
    }
    struct vtree_lookup_entry *le, *ltmp;
    HASH_ITER(hh, vt->by_parent, le, ltmp) {
        HASH_DEL(vt->by_parent, le);
        free(le);
    }

    pthread_mutex_unlock(&vt->lock);
    pthread_mutex_destroy(&vt->lock);
}

uint64_t vtree_lookup_child(struct vtree_ctx *vt, uint64_t parent_vino,
                            const char *name)
{
    char key[80];
    snprintf(key, sizeof(key), "%llu:%s",
             (unsigned long long)parent_vino, name);

    pthread_mutex_lock(&vt->lock);
    struct vtree_lookup_entry *le;
    HASH_FIND_STR(vt->by_parent, key, le);
    uint64_t result = le ? le->vino : 0;
    pthread_mutex_unlock(&vt->lock);
    return result;
}

struct vtree_node *vtree_get(struct vtree_ctx *vt, uint64_t vino)
{
    pthread_mutex_lock(&vt->lock);
    struct vtree_node *n;
    HASH_FIND(hh, vt->by_ino, &vino, sizeof(uint64_t), n);
    pthread_mutex_unlock(&vt->lock);
    return n;
}

static uint64_t vtree_alloc(struct vtree_ctx *vt, uint64_t parent_vino,
                            const char *name, uint64_t real_ino,
                            int is_version_file, uint64_t version)
{
    char key[80];
    snprintf(key, sizeof(key), "%llu:%s",
             (unsigned long long)parent_vino, name);

    pthread_mutex_lock(&vt->lock);

    /* Idempotent: return existing if already allocated */
    struct vtree_lookup_entry *existing;
    HASH_FIND_STR(vt->by_parent, key, existing);
    if (existing) {
        uint64_t vino = existing->vino;
        pthread_mutex_unlock(&vt->lock);
        return vino;
    }

    uint64_t vino = vt->next_vino++;

    struct vtree_node *n = calloc(1, sizeof(*n));
    if (!n) { pthread_mutex_unlock(&vt->lock); return 0; }
    n->vino            = vino;
    n->real_ino        = real_ino;
    n->is_version_file = is_version_file;
    n->version         = version;
    HASH_ADD(hh, vt->by_ino, vino, sizeof(uint64_t), n);

    struct vtree_lookup_entry *le = calloc(1, sizeof(*le));
    if (!le) { pthread_mutex_unlock(&vt->lock); return vino; } /* node added, skip reverse */
    strncpy(le->key, key, sizeof(le->key) - 1);
    le->vino = vino;
    HASH_ADD_STR(vt->by_parent, key, le);

    pthread_mutex_unlock(&vt->lock);
    return vino;
}

uint64_t vtree_alloc_dir(struct vtree_ctx *vt, uint64_t parent_vino,
                         const char *name, uint64_t real_ino)
{
    return vtree_alloc(vt, parent_vino, name, real_ino, 0, 0);
}

uint64_t vtree_alloc_vfile(struct vtree_ctx *vt, uint64_t parent_vino,
                           const char *name, uint64_t real_ino, uint64_t version)
{
    return vtree_alloc(vt, parent_vino, name, real_ino, 1, version);
}
```

**Step 2: Commit**

```bash
git add src/vfs_versions.c
git commit -m "feat: implement vtree session-local virtual inode table"
```

---

### Task 3: Wire vtree into global context

**Files:**
- Modify: `src/kvbfs.h`
- Modify: `src/context.c`

**Step 1: Add include and field to `kvbfs.h`**

After the existing `#include "uthash.h"` line (line 13), add:

```c
#include "vfs_versions.h"
```

After the `struct kvbfs_super super;` field in `struct kvbfs_ctx` (line 72), add:

```c
    struct vtree_ctx vtree;             /* Version virtual directory tree */
```

**Step 2: Init and destroy in `context.c`**

In `ctx_init`, after `ctx->icache = NULL;` (line 91), add:

```c
    vtree_init(&ctx->vtree);
```

In `ctx_destroy`, before `inode_sync_all();` (line 183), add:

```c
    vtree_destroy(&ctx->vtree);
```

**Step 3: Commit**

```bash
git add src/kvbfs.h src/context.c
git commit -m "feat: add vtree_ctx to kvbfs_ctx, init/destroy in context"
```

---

### Task 4: Add `vfs_versions.c` to build

**Files:**
- Modify: `CMakeLists.txt`

**Step 1: Add to KVBFS_SOURCES**

Find the `set(KVBFS_SOURCES` block (around line 118). Add `src/vfs_versions.c` before the closing `)`:

```cmake
set(KVBFS_SOURCES
    src/main.c
    src/fuse_ops.c
    src/inode.c
    src/context.c
    src/super.c
    src/kv_store.c
    src/version.c
    src/utils.c
    src/vfs_versions.c        # ← add this line
    ${LLM_SOURCES}
    ${MEM_SOURCES}
)
```

**Step 2: Verify it compiles**

```bash
cmake -B build && make -C build -j$(nproc) 2>&1 | tail -5
```

Expected: `[100%] Built target kvbfs` (no errors)

**Step 3: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: add vfs_versions.c to KVBFS_SOURCES"
```

---

### Task 5: Write failing E2E tests (Tests 52–57)

**Files:**
- Modify: `tests/test_kvbfs.sh`

**Step 1: Append tests after Test 51**

Find the end of Test 51 block (the `.events survives rm attempt` test) and add:

```bash
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
if echo "$VER_LIST" | grep -qE '^[0-9]+$|[[:space:]][0-9]+'; then
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
echo "should fail" > "$MNT/.versions/ver_test.txt/1" 2>/dev/null
RET=$?
if [ "$RET" -ne 0 ]; then
    pass ".versions write rejected"
else
    fail ".versions write protection" "write succeeded unexpectedly"
fi
```

**Step 2: Run tests to confirm they fail (expected)**

```bash
# Mount filesystem first
mkdir -p /tmp/kvbfs_e2e_mount
./build/kvbfs /tmp/kvbfs_e2e_mount -f &
sleep 1
bash tests/test_kvbfs.sh /tmp/kvbfs_e2e_mount ./build/kvbfs 2>&1 | grep -A1 "Test 5[2-7]"
fusermount -u /tmp/kvbfs_e2e_mount
```

Expected: Tests 52–57 all FAIL (`.versions` not yet visible)

**Step 3: Commit**

```bash
git add tests/test_kvbfs.sh
git commit -m "test: add E2E tests 52-57 for .versions virtual directory (failing)"
```

---

### Task 6: `fuse_ops.c` — Add `.versions` helper and `lookup`

**Files:**
- Modify: `src/fuse_ops.c`

**Step 1: Add includes and stat helper**

At the top of `fuse_ops.c`, after the existing `#ifdef CFS_MEMORY` block of includes, add unconditionally:

```c
#include "vfs_versions.h"
#include "version.h"
#include "inode.h"
#include "kv_store.h"
```

(Most of these are already included — add any that aren't present.)

After the existing `agentfs_events_stat` function, add:

```c
static void versions_root_stat(struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino   = AGENTFS_VERSIONS_INO;
    st->st_mode  = S_IFDIR | 0555;
    st->st_nlink = 2;
    st->st_uid   = getuid();
    st->st_gid   = getgid();
    clock_gettime(CLOCK_REALTIME, &st->st_atim);
    st->st_mtim  = st->st_atim;
    st->st_ctim  = st->st_atim;
}

static void vnode_stat(struct vtree_node *vn, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino  = vn->vino;
    st->st_uid  = getuid();
    st->st_gid  = getgid();

    if (!vn->is_version_file) {
        st->st_mode  = S_IFDIR | 0555;
        st->st_nlink = 2;
        clock_gettime(CLOCK_REALTIME, &st->st_atim);
        st->st_mtim  = st->st_atim;
    } else {
        struct kvbfs_version_meta meta;
        st->st_mode  = S_IFREG | 0444;
        st->st_nlink = 1;
        if (version_get_meta(vn->real_ino, vn->version, &meta) == 0) {
            st->st_size  = (off_t)meta.size;
            st->st_blocks = (blkcnt_t)((meta.size + 511) / 512);
            st->st_mtim  = meta.mtime;
        }
        clock_gettime(CLOCK_REALTIME, &st->st_atim);
    }
    st->st_ctim = st->st_atim;
}
```

**Step 2: Update `kvbfs_lookup`**

Inside `kvbfs_lookup`, after the existing `#ifdef CFS_MEMORY` block for `.events` lookup (after line ~378), add before the `uint64_t child_ino = dirent_lookup(...)` line:

```c
    /* Virtual .versions root */
    if (parent == KVBFS_ROOT_INO && strcmp(name, AGENTFS_VERSIONS_NAME) == 0) {
        struct fuse_entry_param e;
        memset(&e, 0, sizeof(e));
        e.ino           = AGENTFS_VERSIONS_INO;
        e.attr_timeout  = 0;
        e.entry_timeout = 0;
        versions_root_stat(&e.attr);
        fuse_reply_entry(req, &e);
        return;
    }

    /* Child lookup within .versions virtual tree */
    if (parent == AGENTFS_VERSIONS_INO || vtree_is_vnode(parent)) {
        /* Determine the real inode of the parent directory/file */
        uint64_t parent_real_ino;
        if (parent == AGENTFS_VERSIONS_INO) {
            parent_real_ino = KVBFS_ROOT_INO;
        } else {
            struct vtree_node *pn = vtree_get(&g_ctx->vtree, parent);
            if (!pn) { fuse_reply_err(req, ENOENT); return; }
            parent_real_ino = pn->real_ino;
        }

        /* Load parent real inode to determine if it is a dir or regular file */
        struct kvbfs_inode parent_inode;
        int parent_is_dir = 0;
        if (inode_load(parent_real_ino, &parent_inode) == 0)
            parent_is_dir = S_ISDIR(parent_inode.mode);

        uint64_t vino;
        struct stat st;

        if (parent_is_dir) {
            /* Lookup child in real KV dirents */
            uint64_t child_real_ino = dirent_lookup(parent_real_ino, name);
            if (child_real_ino == 0) { fuse_reply_err(req, ENOENT); return; }
            vino = vtree_alloc_dir(&g_ctx->vtree, parent, name, child_real_ino);
            if (!vino) { fuse_reply_err(req, ENOMEM); return; }
            struct vtree_node *cn = vtree_get(&g_ctx->vtree, vino);
            vnode_stat(cn, &st);
        } else {
            /* Parent is a file → children are version numbers */
            char *endptr;
            uint64_t ver = strtoull(name, &endptr, 10);
            if (*endptr != '\0' || ver == 0) { fuse_reply_err(req, ENOENT); return; }
            struct kvbfs_version_meta vmeta;
            if (version_get_meta(parent_real_ino, ver, &vmeta) != 0) {
                fuse_reply_err(req, ENOENT); return;
            }
            vino = vtree_alloc_vfile(&g_ctx->vtree, parent, name,
                                     parent_real_ino, ver);
            if (!vino) { fuse_reply_err(req, ENOMEM); return; }
            struct vtree_node *cn = vtree_get(&g_ctx->vtree, vino);
            vnode_stat(cn, &st);
        }

        struct fuse_entry_param e;
        memset(&e, 0, sizeof(e));
        e.ino           = vino;
        e.attr_timeout  = 0;
        e.entry_timeout = 0;
        e.attr          = st;
        fuse_reply_entry(req, &e);
        return;
    }
```

**Step 3: Commit**

```bash
git add src/fuse_ops.c
git commit -m "feat: implement .versions lookup in FUSE"
```

---

### Task 7: `fuse_ops.c` — `getattr` and `setattr`

**Files:**
- Modify: `src/fuse_ops.c`

**Step 1: Update `kvbfs_getattr`**

After the existing block for `AGENTFS_EVENTS_INO` in `kvbfs_getattr` (around line 425), add:

```c
    if (ino == AGENTFS_VERSIONS_INO) {
        struct stat st;
        versions_root_stat(&st);
        fuse_reply_attr(req, &st, 0);
        return;
    }
    if (vtree_is_vnode(ino)) {
        struct vtree_node *vn = vtree_get(&g_ctx->vtree, ino);
        if (!vn) { fuse_reply_err(req, ENOENT); return; }
        struct stat st;
        vnode_stat(vn, &st);
        fuse_reply_attr(req, &st, 0);
        return;
    }
```

**Step 2: Update `kvbfs_setattr`**

After the existing block for `AGENTFS_EVENTS_INO` in `kvbfs_setattr`, add the same guard (virtual tree is read-only, just return current attr):

```c
    if (ino == AGENTFS_VERSIONS_INO) {
        struct stat st;
        versions_root_stat(&st);
        fuse_reply_attr(req, &st, 0);
        return;
    }
    if (vtree_is_vnode(ino)) {
        struct vtree_node *vn = vtree_get(&g_ctx->vtree, ino);
        if (!vn) { fuse_reply_err(req, ENOENT); return; }
        struct stat st;
        vnode_stat(vn, &st);
        fuse_reply_attr(req, &st, 0);
        return;
    }
```

**Step 3: Commit**

```bash
git add src/fuse_ops.c
git commit -m "feat: implement getattr/setattr for .versions virtual tree"
```

---

### Task 8: `fuse_ops.c` — `opendir` and `readdir`

**Files:**
- Modify: `src/fuse_ops.c`

**Step 1: Update `kvbfs_opendir`**

Add early return before `inode_get(ino)` in `kvbfs_opendir`:

```c
    if (ino == AGENTFS_VERSIONS_INO || vtree_is_vnode(ino)) {
        fuse_reply_open(req, fi);
        return;
    }
```

**Step 2: Update `kvbfs_readdir`**

Add a new branch at the top of `kvbfs_readdir` before `inode_get(ino)`:

```c
    /* .versions root: enumerate real root dirents as virtual dir entries */
    if (ino == AGENTFS_VERSIONS_INO) {
        char *buf = malloc(size);
        if (!buf) { fuse_reply_err(req, ENOMEM); return; }
        size_t buf_used = 0;
        off_t entry_idx = 0;

        /* . and .. */
        if (off <= 0) {
            struct stat st; versions_root_stat(&st);
            size_t es = fuse_add_direntry(req, buf + buf_used, size - buf_used,
                                          ".", &st, ++entry_idx);
            if (es <= size - buf_used) buf_used += es;
        }
        if (off <= 1) {
            struct stat st = {.st_ino = KVBFS_ROOT_INO, .st_mode = S_IFDIR};
            size_t es = fuse_add_direntry(req, buf + buf_used, size - buf_used,
                                          "..", &st, ++entry_idx);
            if (es <= size - buf_used) buf_used += es;
        }

        /* Enumerate real root directory entries */
        char prefix[64];
        int prefix_len = kvbfs_key_dirent_prefix(prefix, sizeof(prefix),
                                                  KVBFS_ROOT_INO);
        kv_iterator_t *iter = kv_iter_prefix(g_ctx->db, prefix, prefix_len);
        while (kv_iter_valid(iter)) {
            entry_idx++;
            if (entry_idx <= off) { kv_iter_next(iter); continue; }

            size_t klen;
            const char *k = kv_iter_key(iter, &klen);
            const char *name = k + prefix_len;
            size_t nlen = klen - prefix_len;

            size_t vlen;
            const char *val = kv_iter_value(iter, &vlen);
            uint64_t child_real_ino = 0;
            if (vlen == sizeof(uint64_t)) memcpy(&child_real_ino, val, sizeof(uint64_t));

            char nbuf[256];
            if (nlen >= sizeof(nbuf)) nlen = sizeof(nbuf) - 1;
            memcpy(nbuf, name, nlen); nbuf[nlen] = '\0';

            uint64_t vino = vtree_alloc_dir(&g_ctx->vtree, AGENTFS_VERSIONS_INO,
                                            nbuf, child_real_ino);
            struct stat st = {.st_ino = vino, .st_mode = S_IFDIR | 0555};
            size_t es = fuse_add_direntry(req, buf + buf_used, size - buf_used,
                                          nbuf, &st, entry_idx + 1);
            if (es > size - buf_used) { kv_iter_free(iter); goto vdone; }
            buf_used += es;
            kv_iter_next(iter);
        }
        kv_iter_free(iter);
    vdone:
        fuse_reply_buf(req, buf, buf_used);
        free(buf);
        return;
    }

    /* Virtual tree node readdir */
    if (vtree_is_vnode(ino)) {
        struct vtree_node *vn = vtree_get(&g_ctx->vtree, ino);
        if (!vn || vn->is_version_file) { fuse_reply_err(req, ENOTDIR); return; }

        char *buf = malloc(size);
        if (!buf) { fuse_reply_err(req, ENOMEM); return; }
        size_t buf_used = 0;
        off_t entry_idx = 0;

        /* . and .. */
        if (off <= 0) {
            struct stat st; vnode_stat(vn, &st);
            size_t es = fuse_add_direntry(req, buf + buf_used, size - buf_used,
                                          ".", &st, ++entry_idx);
            if (es <= size - buf_used) buf_used += es;
        }
        if (off <= 1) {
            struct stat st = {.st_ino = ino, .st_mode = S_IFDIR};
            size_t es = fuse_add_direntry(req, buf + buf_used, size - buf_used,
                                          "..", &st, ++entry_idx);
            if (es <= size - buf_used) buf_used += es;
        }

        /* Determine if real_ino is a dir or file */
        struct kvbfs_inode ri;
        int is_dir = 0;
        if (inode_load(vn->real_ino, &ri) == 0) is_dir = S_ISDIR(ri.mode);

        if (is_dir) {
            /* Enumerate real directory children */
            char prefix[64];
            int plen = kvbfs_key_dirent_prefix(prefix, sizeof(prefix), vn->real_ino);
            kv_iterator_t *iter = kv_iter_prefix(g_ctx->db, prefix, plen);
            while (kv_iter_valid(iter)) {
                entry_idx++;
                if (entry_idx <= off) { kv_iter_next(iter); continue; }
                size_t klen;
                const char *k = kv_iter_key(iter, &klen);
                const char *name = k + plen;
                size_t nlen = klen - plen;
                size_t vlen;
                const char *val = kv_iter_value(iter, &vlen);
                uint64_t child_real = 0;
                if (vlen == sizeof(uint64_t)) memcpy(&child_real, val, sizeof(uint64_t));

                char nbuf[256];
                if (nlen >= sizeof(nbuf)) nlen = sizeof(nbuf) - 1;
                memcpy(nbuf, name, nlen); nbuf[nlen] = '\0';

                uint64_t cvino = vtree_alloc_dir(&g_ctx->vtree, ino, nbuf, child_real);
                struct stat st = {.st_ino = cvino, .st_mode = S_IFDIR | 0555};
                size_t es = fuse_add_direntry(req, buf + buf_used, size - buf_used,
                                              nbuf, &st, entry_idx + 1);
                if (es > size - buf_used) { kv_iter_free(iter); goto vdone2; }
                buf_used += es;
                kv_iter_next(iter);
            }
            kv_iter_free(iter);
        } else {
            /* real_ino is a file — enumerate version numbers */
            char prefix[64];
            int plen = kvbfs_key_version_meta_prefix(prefix, sizeof(prefix),
                                                     vn->real_ino);
            kv_iterator_t *iter = kv_iter_prefix(g_ctx->db, prefix, plen);
            while (kv_iter_valid(iter)) {
                entry_idx++;
                if (entry_idx <= off) { kv_iter_next(iter); continue; }

                size_t klen;
                const char *k = kv_iter_key(iter, &klen);
                /* Key format: "vm:<ino>:<ver>" — extract version number */
                const char *ver_str = k + plen;  /* points past "vm:<ino>:" */
                size_t ver_len = klen - plen;

                char nbuf[32];
                if (ver_len >= sizeof(nbuf)) ver_len = sizeof(nbuf) - 1;
                memcpy(nbuf, ver_str, ver_len); nbuf[ver_len] = '\0';
                uint64_t ver = strtoull(nbuf, NULL, 10);

                uint64_t cvino = vtree_alloc_vfile(&g_ctx->vtree, ino, nbuf,
                                                   vn->real_ino, ver);
                struct stat st = {.st_ino = cvino, .st_mode = S_IFREG | 0444};
                size_t es = fuse_add_direntry(req, buf + buf_used, size - buf_used,
                                              nbuf, &st, entry_idx + 1);
                if (es > size - buf_used) { kv_iter_free(iter); goto vdone2; }
                buf_used += es;
                kv_iter_next(iter);
            }
            kv_iter_free(iter);
        }
    vdone2:
        fuse_reply_buf(req, buf, buf_used);
        free(buf);
        return;
    }
```

Also, in the root `readdir` block where `.agentfs` and `.events` are appended (end of `kvbfs_readdir`), add `.versions`:

```c
        /* .versions */
        entry_idx++;
        if (entry_idx > off) {
            struct stat ver_st;
            versions_root_stat(&ver_st);
            size_t entsize = fuse_add_direntry(req, buf + buf_used, size - buf_used,
                                               AGENTFS_VERSIONS_NAME, &ver_st,
                                               entry_idx + 1);
            if (entsize <= size - buf_used)
                buf_used += entsize;
        }
```

**Step 3: Commit**

```bash
git add src/fuse_ops.c
git commit -m "feat: implement readdir for .versions virtual tree"
```

---

### Task 9: `fuse_ops.c` — `open`, `read`, `release` for version files

**Files:**
- Modify: `src/fuse_ops.c`

**Step 1: Update `kvbfs_open`**

After the existing `AGENTFS_EVENTS_INO` block in `kvbfs_open`, add:

```c
    if (ino == AGENTFS_VERSIONS_INO) {
        fuse_reply_err(req, EISDIR);
        return;
    }
    if (vtree_is_vnode(ino)) {
        struct vtree_node *vn = vtree_get(&g_ctx->vtree, ino);
        if (!vn) { fuse_reply_err(req, ENOENT); return; }
        if (!vn->is_version_file) { fuse_reply_err(req, EISDIR); return; }
        if ((fi->flags & O_ACCMODE) != O_RDONLY) {
            fuse_reply_err(req, EACCES);
            return;
        }
        struct version_fh *vfh = calloc(1, sizeof(*vfh));
        if (!vfh) { fuse_reply_err(req, ENOMEM); return; }
        vfh->real_ino = vn->real_ino;
        vfh->version  = vn->version;
        fi->fh        = (uint64_t)(uintptr_t)vfh;
        fi->direct_io = 1;
        fuse_reply_open(req, fi);
        return;
    }
```

**Step 2: Update `kvbfs_read`**

After the existing `AGENTFS_EVENTS_INO` block in `kvbfs_read`, add:

```c
    if (vtree_is_vnode(ino)) {
        struct version_fh *vfh = (struct version_fh *)(uintptr_t)fi->fh;
        if (!vfh) { fuse_reply_err(req, EIO); return; }

        /* Calculate which blocks cover [off, off+size) */
        uint64_t start_block = off / KVBFS_BLOCK_SIZE;
        uint64_t end_block   = (off + size - 1) / KVBFS_BLOCK_SIZE;

        char *outbuf = calloc(1, size);
        if (!outbuf) { fuse_reply_err(req, ENOMEM); return; }
        size_t copied = 0;

        for (uint64_t blk = start_block; blk <= end_block && copied < size; blk++) {
            char *data = NULL;
            size_t dlen = 0;
            if (version_read_block(vfh->real_ino, vfh->version, blk,
                                   &data, &dlen) != 0)
                break;

            /* Offset within this block */
            size_t blk_off = (blk == start_block) ? (off % KVBFS_BLOCK_SIZE) : 0;
            size_t avail   = dlen > blk_off ? dlen - blk_off : 0;
            size_t tocopy  = avail < (size - copied) ? avail : (size - copied);

            memcpy(outbuf + copied, data + blk_off, tocopy);
            copied += tocopy;
            free(data);
        }

        fuse_reply_buf(req, outbuf, copied);
        free(outbuf);
        return;
    }
```

**Step 3: Update `kvbfs_release`**

After the existing `AGENTFS_EVENTS_INO` block in `kvbfs_release`, add:

```c
    if (vtree_is_vnode(ino)) {
        struct version_fh *vfh = (struct version_fh *)(uintptr_t)fi->fh;
        free(vfh);
        fuse_reply_err(req, 0);
        return;
    }
```

**Step 4: Commit**

```bash
git add src/fuse_ops.c
git commit -m "feat: implement open/read/release for version files in .versions"
```

---

### Task 10: Write-protect the virtual tree

**Files:**
- Modify: `src/fuse_ops.c`

**Step 1: Block writes**

In `kvbfs_write`, before any other checks at the top of the function, add:

```c
    if (ino == AGENTFS_VERSIONS_INO || vtree_is_vnode(ino)) {
        fuse_reply_err(req, EACCES);
        return;
    }
```

In `kvbfs_unlink`, add after the virtual CTL/EVENTS guards:

```c
    if (vtree_is_vnode(parent)) {
        fuse_reply_err(req, EACCES);
        return;
    }
```

In `kvbfs_setxattr`, `kvbfs_getxattr`, `kvbfs_listxattr`, `kvbfs_removexattr`, add alongside existing CTL/EVENTS guards:

```c
    if (ino == AGENTFS_VERSIONS_INO || vtree_is_vnode(ino)) {
        fuse_reply_err(req, ENOTSUP);
        return;
    }
```

**Step 2: Commit**

```bash
git add src/fuse_ops.c
git commit -m "feat: write-protect .versions virtual tree (EACCES/ENOTSUP)"
```

---

### Task 11: Build, run tests, commit

**Step 1: Build**

```bash
cmake -B build && make -C build -j$(nproc) 2>&1 | tail -5
```

Expected: no errors.

**Step 2: Run full E2E test suite**

```bash
mkdir -p /tmp/kvbfs_e2e_mount
./build/kvbfs /tmp/kvbfs_e2e_mount -f &
sleep 1
bash tests/test_kvbfs.sh /tmp/kvbfs_e2e_mount ./build/kvbfs
fusermount -u /tmp/kvbfs_e2e_mount
```

Expected: All 57 tests PASS.

**Step 3: If any test fails**

Check FUSE debug output by running with `-d` flag and re-running the specific failing test step manually to isolate the issue.

**Step 4: Update README**

In `README.md`, in the E2E test coverage table, update the last row and add:

```markdown
| .events 变更通知 | 47-51 | 5 |
| .versions 版本目录 | 52-57 | 6 |
```

Also add a `### 版本历史访问` section under `### 自动版本快照`:

```markdown
### 版本历史访问

通过虚拟目录 `/.versions/` 可以用标准 POSIX 操作读取文件历史版本：

```bash
# 列出所有历史版本
ls /.versions/workspace/notes.md/

# 读取版本 2 的内容
cat /.versions/workspace/notes.md/2

# 恢复版本 1（产生新的版本快照）
cp /.versions/workspace/notes.md/1 /workspace/notes.md
```

支持任意深度子目录：`/.versions/<完整路径>/<版本号>`
```

**Step 5: Final commit**

```bash
git add README.md
git commit -m "docs: document .versions virtual directory in README"
```
