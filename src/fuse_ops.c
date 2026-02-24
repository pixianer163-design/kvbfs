#include "kvbfs.h"
#include "kv_store.h"
#include "inode.h"
#include "version.h"

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include "super.h"
#include <unistd.h>
#include <sys/stat.h>
#include <poll.h>
#include <sys/xattr.h>

#ifdef CFS_LOCAL_LLM
#include "llm.h"
static int is_session_file(fuse_ino_t ino);
#endif

static void xattr_delete_all(uint64_t ino);

/* ── Virtual .agentfs control file ───────────────────── */
#ifdef CFS_MEMORY

/* Per-open state for the .agentfs control file */
struct agentfs_ctl_fh {
    char  *query;        /* accumulated write data (query text) */
    size_t query_len;
    char  *result;       /* JSON result from search */
    size_t result_len;
};

static void agentfs_ctl_stat(struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino = AGENTFS_CTL_INO;
    st->st_mode = S_IFREG | 0660;
    st->st_nlink = 1;
    st->st_size = 0;
    st->st_uid = getuid();
    st->st_gid = getgid();
    clock_gettime(CLOCK_REALTIME, &st->st_atim);
    st->st_mtim = st->st_atim;
    st->st_ctim = st->st_atim;
}

static void agentfs_events_stat(struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino = AGENTFS_EVENTS_INO;
    st->st_mode = S_IFREG | 0440;
    st->st_nlink = 1;
    st->st_size = 0;
    st->st_uid = getuid();
    st->st_gid = getgid();
    clock_gettime(CLOCK_REALTIME, &st->st_atim);
    st->st_mtim = st->st_atim;
    st->st_ctim = st->st_atim;
}

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
    st->st_ino = vn->vino;
    st->st_uid = getuid();
    st->st_gid = getgid();

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
            st->st_size   = (off_t)meta.size;
            st->st_blocks = (blkcnt_t)((meta.size + 511) / 512);
            st->st_mtim   = meta.mtime;
        }
        clock_gettime(CLOCK_REALTIME, &st->st_atim);
    }
    st->st_ctim = st->st_atim;
}

/* Resolve inode number to a path by walking dirent entries backwards */
static int ino_to_path(uint64_t ino, char *buf, size_t buflen)
{
    if (ino == KVBFS_ROOT_INO) {
        if (buflen < 2) return -1;
        buf[0] = '/';
        buf[1] = '\0';
        return 0;
    }

    /* Build path components by searching parent dirents */
    char *components[128];
    int depth = 0;
    uint64_t cur = ino;

    while (cur != KVBFS_ROOT_INO && depth < 128) {
        /* Scan all dirent prefixes to find parent -> cur mapping */
        bool found = false;
        kv_iterator_t *iter = kv_iter_prefix(g_ctx->db, "d:", 2);
        while (kv_iter_valid(iter)) {
            size_t vlen;
            const char *val = kv_iter_value(iter, &vlen);
            if (vlen == sizeof(uint64_t)) {
                uint64_t child;
                memcpy(&child, val, sizeof(uint64_t));
                if (child == cur) {
                    size_t klen;
                    const char *key = kv_iter_key(iter, &klen);
                    /* key = "d:<parent>:<name>" */
                    const char *p = key + 2;  /* skip "d:" */
                    const char *colon = memchr(p, ':', klen - 2);
                    if (colon) {
                        const char *name = colon + 1;
                        size_t nlen = klen - (name - key);
                        components[depth] = strndup(name, nlen);
                        depth++;

                        /* Parse parent ino */
                        char parent_str[32];
                        size_t plen = colon - p;
                        if (plen >= sizeof(parent_str)) plen = sizeof(parent_str) - 1;
                        memcpy(parent_str, p, plen);
                        parent_str[plen] = '\0';
                        cur = strtoull(parent_str, NULL, 10);
                        found = true;
                        kv_iter_free(iter);
                        break;
                    }
                }
            }
            kv_iter_next(iter);
        }
        if (!found) {
            kv_iter_free(iter);
            break;
        }
    }

    if (depth == 0) {
        snprintf(buf, buflen, "ino:%lu", (unsigned long)ino);
        return -1;
    }

    /* Build path from components (reverse order) */
    size_t off = 0;
    for (int i = depth - 1; i >= 0; i--) {
        int n = snprintf(buf + off, buflen - off, "/%s", components[i]);
        if (n < 0 || (size_t)n >= buflen - off) break;
        off += n;
    }
    for (int i = 0; i < depth; i++) free(components[i]);
    return 0;
}

/* Execute search query and format JSON results */
static int agentfs_ctl_search(struct agentfs_ctl_fh *fh)
{
    if (!fh->query || fh->query_len == 0) {
        /* No query — return usage info */
        const char *usage =
            "{\"status\":\"ready\",\"usage\":\"Write a search query, then read results.\"}\n";
        fh->result = strdup(usage);
        fh->result_len = strlen(usage);
        return 0;
    }

    /* Null-terminate query */
    char *q = malloc(fh->query_len + 1);
    if (!q) return -1;
    memcpy(q, fh->query, fh->query_len);
    q[fh->query_len] = '\0';

    /* Strip trailing whitespace/newline */
    size_t qlen = fh->query_len;
    while (qlen > 0 && (q[qlen - 1] == '\n' || q[qlen - 1] == '\r' ||
                         q[qlen - 1] == ' '))
        q[--qlen] = '\0';

    if (qlen == 0) {
        free(q);
        const char *empty = "{\"status\":\"error\",\"message\":\"empty query\"}\n";
        fh->result = strdup(empty);
        fh->result_len = strlen(empty);
        return 0;
    }

    /* Perform search */
    struct cfs_mem_query query;
    memset(&query, 0, sizeof(query));
    strncpy(query.query_text, q, sizeof(query.query_text) - 1);
    query.top_k = 10;
    free(q);

    int ret = mem_search(&g_ctx->mem, g_ctx->db, &query);
    if (ret != 0) {
        const char *err = "{\"status\":\"error\",\"message\":\"search failed\"}\n";
        fh->result = strdup(err);
        fh->result_len = strlen(err);
        return 0;
    }

    /* Format JSON output */
    size_t cap = 4096;
    char *json = malloc(cap);
    if (!json) return -1;

    int off = snprintf(json, cap, "{\"status\":\"ok\",\"results\":[");

    for (int i = 0; i < query.n_results; i++) {
        struct cfs_mem_result *r = &query.results[i];

        /* Resolve ino to path */
        char path[CFS_MEM_PATH_LEN];
        ino_to_path(r->ino, path, sizeof(path));

        /* Escape summary for JSON (simple: replace " and \ and control chars) */
        char escaped[CFS_MEM_SUMMARY_LEN * 2];
        size_t ei = 0;
        for (size_t si = 0; r->summary[si] && ei < sizeof(escaped) - 2; si++) {
            char c = r->summary[si];
            if (c == '"' || c == '\\') {
                escaped[ei++] = '\\';
                escaped[ei++] = c;
            } else if (c == '\n') {
                escaped[ei++] = '\\';
                escaped[ei++] = 'n';
            } else if (c == '\r') {
                escaped[ei++] = '\\';
                escaped[ei++] = 'r';
            } else if (c == '\t') {
                escaped[ei++] = '\\';
                escaped[ei++] = 't';
            } else if ((unsigned char)c >= 0x20) {
                escaped[ei++] = c;
            }
        }
        escaped[ei] = '\0';

        /* Grow buffer if needed */
        size_t needed = off + ei + 256 + strlen(path);
        if (needed > cap) {
            cap = needed * 2;
            json = realloc(json, cap);
            if (!json) return -1;
        }

        off += snprintf(json + off, cap - off,
                        "%s{\"score\":%.4f,\"ino\":%lu,\"seq\":%u,"
                        "\"path\":\"%s\",\"summary\":\"%s\"}",
                        i > 0 ? "," : "",
                        r->score,
                        (unsigned long)r->ino,
                        r->seq,
                        path,
                        escaped);
    }

    off += snprintf(json + off, cap - off, "]}\n");

    fh->result = json;
    fh->result_len = off;
    return 0;
}

#endif /* CFS_MEMORY */

/* FUSE lowlevel 操作实现 */

static void inode_to_stat(const struct kvbfs_inode *inode, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino = inode->ino;
    st->st_mode = inode->mode;
    st->st_nlink = inode->nlink;
    st->st_size = inode->size;
    st->st_blocks = inode->blocks * (KVBFS_BLOCK_SIZE / 512);
    st->st_blksize = KVBFS_BLOCK_SIZE;
    st->st_atim = inode->atime;
    st->st_mtim = inode->mtime;
    st->st_ctim = inode->ctime;
    st->st_uid = getuid();
    st->st_gid = getgid();
}

/* 查找目录项，返回子 inode 号，未找到返回 0 */
static uint64_t dirent_lookup(uint64_t parent, const char *name)
{
    char key[KVBFS_KEY_MAX];
    int keylen = kvbfs_key_dirent(key, sizeof(key), parent, name);
    if (keylen < 0) return 0;  /* key overflow */

    char *value = NULL;
    size_t value_len = 0;

    int ret = kv_get(g_ctx->db, key, keylen, &value, &value_len);
    if (ret != 0 || value_len != sizeof(uint64_t)) {
        if (value) free(value);
        return 0;
    }

    uint64_t child_ino;
    memcpy(&child_ino, value, sizeof(uint64_t));
    free(value);
    return child_ino;
}

/* 添加目录项 */
static int dirent_add(uint64_t parent, const char *name, uint64_t child)
{
    char key[KVBFS_KEY_MAX];
    int keylen = kvbfs_key_dirent(key, sizeof(key), parent, name);
    if (keylen < 0) return -1;  /* key overflow */

    return kv_put(g_ctx->db, key, keylen,
                  (const char *)&child, sizeof(uint64_t));
}

/* 删除目录项 */
static int dirent_remove(uint64_t parent, const char *name)
{
    char key[KVBFS_KEY_MAX];
    int keylen = kvbfs_key_dirent(key, sizeof(key), parent, name);
    if (keylen < 0) return -1;  /* key overflow */

    return kv_delete(g_ctx->db, key, keylen);
}

static int dirent_is_empty(uint64_t ino)
{
    char prefix[64];
    int prefix_len = kvbfs_key_dirent_prefix(prefix, sizeof(prefix), ino);

    kv_iterator_t *iter = kv_iter_prefix(g_ctx->db, prefix, prefix_len);
    int empty = !kv_iter_valid(iter);
    kv_iter_free(iter);

    return empty;
}

static void delete_file_blocks(uint64_t ino, uint64_t blocks)
{
    for (uint64_t i = 0; i < blocks; i++) {
        char key[64];
        int keylen = kvbfs_key_block(key, sizeof(key), ino, i);
        kv_delete(g_ctx->db, key, keylen);
    }
}

static void kvbfs_init(void *userdata, struct fuse_conn_info *conn)
{
    (void)userdata;
    (void)conn;

    /* 上下文已在 main.c 中初始化 */
    printf("KVBFS initialized\n");
}

static void kvbfs_destroy(void *userdata)
{
    (void)userdata;

    printf("KVBFS shutting down...\n");

    /* 同步所有脏 inode */
    inode_sync_all();

    /* 清理缓存 */
    inode_cache_clear();

    printf("KVBFS shutdown complete\n");
}

static void kvbfs_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
#ifdef CFS_MEMORY
    /* Virtual .agentfs control file in root */
    if (parent == KVBFS_ROOT_INO && strcmp(name, AGENTFS_CTL_NAME) == 0) {
        struct fuse_entry_param e;
        memset(&e, 0, sizeof(e));
        e.ino = AGENTFS_CTL_INO;
        e.attr_timeout = 0;
        e.entry_timeout = 0;
        agentfs_ctl_stat(&e.attr);
        fuse_reply_entry(req, &e);
        return;
    }
    /* Virtual .events file in root */
    if (parent == KVBFS_ROOT_INO && strcmp(name, AGENTFS_EVENTS_NAME) == 0) {
        struct fuse_entry_param e;
        memset(&e, 0, sizeof(e));
        e.ino = AGENTFS_EVENTS_INO;
        e.attr_timeout = 0;
        e.entry_timeout = 0;
        agentfs_events_stat(&e.attr);
        fuse_reply_entry(req, &e);
        return;
    }
#endif

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
        uint64_t parent_real_ino;
        if (parent == AGENTFS_VERSIONS_INO) {
            parent_real_ino = KVBFS_ROOT_INO;
        } else {
            struct vtree_node *pn = vtree_get(&g_ctx->vtree, parent);
            if (!pn) { fuse_reply_err(req, ENOENT); return; }
            parent_real_ino = pn->real_ino;
        }

        struct kvbfs_inode parent_inode;
        int parent_is_dir = 0;
        if (inode_load(parent_real_ino, &parent_inode) == 0)
            parent_is_dir = S_ISDIR(parent_inode.mode);

        uint64_t vino;
        struct stat st;

        if (parent_is_dir) {
            uint64_t child_real_ino = dirent_lookup(parent_real_ino, name);
            if (child_real_ino == 0) { fuse_reply_err(req, ENOENT); return; }
            vino = vtree_alloc_dir(&g_ctx->vtree, parent, name, child_real_ino);
            if (!vino) { fuse_reply_err(req, ENOMEM); return; }
            struct vtree_node *cn = vtree_get(&g_ctx->vtree, vino);
            vnode_stat(cn, &st);
        } else {
            char *endptr;
            uint64_t uver = strtoull(name, &endptr, 10);
            if (*endptr != '\0' || uver == 0) { fuse_reply_err(req, ENOENT); return; }
            /* Convert 1-indexed user-visible name to 0-indexed internal version */
            uint64_t ver = uver - 1;
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

    uint64_t child_ino = dirent_lookup(parent, name);
    if (child_ino == 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    struct kvbfs_inode_cache *ic = inode_get(child_ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.ino = child_ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;

    pthread_rwlock_rdlock(&ic->lock);
    inode_to_stat(&ic->inode, &e.attr);
    pthread_rwlock_unlock(&ic->lock);

    inode_put(ic);

    fuse_reply_entry(req, &e);
}

static void kvbfs_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    (void)fi;

#ifdef CFS_MEMORY
    if (ino == AGENTFS_CTL_INO) {
        struct stat st;
        agentfs_ctl_stat(&st);
        fuse_reply_attr(req, &st, 0);
        return;
    }
    if (ino == AGENTFS_EVENTS_INO) {
        struct stat st;
        agentfs_events_stat(&st);
        fuse_reply_attr(req, &st, 0);
        return;
    }
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
#endif

    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    struct stat st;
    pthread_rwlock_rdlock(&ic->lock);
    inode_to_stat(&ic->inode, &st);
    pthread_rwlock_unlock(&ic->lock);

    inode_put(ic);

    fuse_reply_attr(req, &st, 1.0);  /* 1 second cache */
}

static void kvbfs_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
                          int to_set, struct fuse_file_info *fi)
{
    (void)fi;

#ifdef CFS_MEMORY
    if (ino == AGENTFS_CTL_INO) {
        /* Virtual file: return current stat, ignore changes */
        struct stat st;
        agentfs_ctl_stat(&st);
        fuse_reply_attr(req, &st, 0);
        return;
    }
    if (ino == AGENTFS_EVENTS_INO) {
        struct stat st;
        agentfs_events_stat(&st);
        fuse_reply_attr(req, &st, 0);
        return;
    }
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
#endif

    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_wrlock(&ic->lock);

    if (to_set & FUSE_SET_ATTR_MODE) {
        ic->inode.mode = (ic->inode.mode & S_IFMT) | (attr->st_mode & 0777);
    }

    if (to_set & FUSE_SET_ATTR_SIZE) {
        uint64_t old_size = ic->inode.size;
        uint64_t new_size = attr->st_size;

        if (new_size < old_size) {
            /* 截断：删除多余的块 */
            uint64_t old_blocks = (old_size + KVBFS_BLOCK_SIZE - 1) / KVBFS_BLOCK_SIZE;
            uint64_t new_blocks = (new_size + KVBFS_BLOCK_SIZE - 1) / KVBFS_BLOCK_SIZE;

            for (uint64_t i = new_blocks; i < old_blocks; i++) {
                char key[64];
                int keylen = kvbfs_key_block(key, sizeof(key), ino, i);
                kv_delete(g_ctx->db, key, keylen);
            }

            /* 零填充最后一个保留块的尾部，避免暴露旧数据 */
            size_t tail_off = new_size % KVBFS_BLOCK_SIZE;
            if (tail_off > 0 && new_blocks > 0) {
                uint64_t last_block = new_blocks - 1;
                char key[64];
                int keylen = kvbfs_key_block(key, sizeof(key), ino, last_block);

                char *block_data = NULL;
                size_t block_len = 0;
                if (kv_get(g_ctx->db, key, keylen, &block_data, &block_len) == 0) {
                    /* 零填充 tail_off 之后的部分 */
                    if (block_len > tail_off) {
                        memset(block_data + tail_off, 0, block_len - tail_off);
                        kv_put(g_ctx->db, key, keylen, block_data, block_len);
                    }
                    free(block_data);
                }
            }
        }

        ic->inode.size = new_size;
        ic->inode.blocks = (new_size + KVBFS_BLOCK_SIZE - 1) / KVBFS_BLOCK_SIZE;
    }

    if (to_set & FUSE_SET_ATTR_ATIME) {
        ic->inode.atime = attr->st_atim;
    }

    if (to_set & FUSE_SET_ATTR_MTIME) {
        ic->inode.mtime = attr->st_mtim;
    }

    if (to_set & (FUSE_SET_ATTR_ATIME_NOW | FUSE_SET_ATTR_MTIME_NOW)) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (to_set & FUSE_SET_ATTR_ATIME_NOW) ic->inode.atime = now;
        if (to_set & FUSE_SET_ATTR_MTIME_NOW) ic->inode.mtime = now;
    }

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    ic->inode.ctime = now;

    struct stat st;
    inode_to_stat(&ic->inode, &st);

    pthread_rwlock_unlock(&ic->lock);

    inode_sync(ic);
    inode_put(ic);

#ifdef CFS_MEMORY
    events_emit(&g_ctx->events, EVT_SETATTR, ino, NULL);
#endif
    fuse_reply_attr(req, &st, 1.0);
}

static void kvbfs_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    if (ino == AGENTFS_VERSIONS_INO || vtree_is_vnode(ino)) {
        fuse_reply_open(req, fi);
        return;
    }

    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_rdlock(&ic->lock);
    int is_dir = S_ISDIR(ic->inode.mode);
    pthread_rwlock_unlock(&ic->lock);

    inode_put(ic);

    if (!is_dir) {
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    fuse_reply_open(req, fi);
}

static void kvbfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                          off_t off, struct fuse_file_info *fi)
{
    (void)fi;

    /* .versions root: list real root entries as virtual dirs */
    if (ino == AGENTFS_VERSIONS_INO) {
        char *buf = malloc(size);
        if (!buf) { fuse_reply_err(req, ENOMEM); return; }
        size_t buf_used = 0;
        off_t entry_idx = 0;

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

        char prefix[64];
        int prefix_len = kvbfs_key_dirent_prefix(prefix, sizeof(prefix), KVBFS_ROOT_INO);
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
            if (es > size - buf_used) { kv_iter_free(iter); goto vr_done; }
            buf_used += es;
            kv_iter_next(iter);
        }
        kv_iter_free(iter);
    vr_done:
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

        struct kvbfs_inode ri;
        int is_dir = 0;
        if (inode_load(vn->real_ino, &ri) == 0) is_dir = S_ISDIR(ri.mode);

        if (is_dir) {
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
                if (es > size - buf_used) { kv_iter_free(iter); goto vn_done; }
                buf_used += es;
                kv_iter_next(iter);
            }
            kv_iter_free(iter);
        } else {
            /* real_ino is a file: enumerate version numbers */
            char prefix[64];
            int plen = kvbfs_key_version_meta_prefix(prefix, sizeof(prefix), vn->real_ino);
            kv_iterator_t *iter = kv_iter_prefix(g_ctx->db, prefix, plen);
            while (kv_iter_valid(iter)) {
                entry_idx++;
                if (entry_idx <= off) { kv_iter_next(iter); continue; }

                size_t klen;
                const char *k = kv_iter_key(iter, &klen);
                const char *ver_str = k + plen;
                size_t ver_len = klen - plen;

                char nbuf[32];
                if (ver_len >= sizeof(nbuf)) ver_len = sizeof(nbuf) - 1;
                memcpy(nbuf, ver_str, ver_len); nbuf[ver_len] = '\0';
                uint64_t ver = strtoull(nbuf, NULL, 10);

                /* Expose 1-indexed names to the user (internal storage is 0-indexed) */
                char display_name[32];
                snprintf(display_name, sizeof(display_name), "%llu",
                         (unsigned long long)(ver + 1));

                uint64_t cvino = vtree_alloc_vfile(&g_ctx->vtree, ino, display_name,
                                                   vn->real_ino, ver);
                struct stat st = {.st_ino = cvino, .st_mode = S_IFREG | 0444};
                size_t es = fuse_add_direntry(req, buf + buf_used, size - buf_used,
                                              display_name, &st, entry_idx + 1);
                if (es > size - buf_used) { kv_iter_free(iter); goto vn_done; }
                buf_used += es;
                kv_iter_next(iter);
            }
            kv_iter_free(iter);
        }
    vn_done:
        fuse_reply_buf(req, buf, buf_used);
        free(buf);
        return;
    }

    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }
    inode_put(ic);

    char *buf = malloc(size);
    if (!buf) {
        fuse_reply_err(req, ENOMEM);
        return;
    }

    size_t buf_used = 0;
    off_t entry_idx = 0;

    /* . and .. entries */
    if (off <= 0) {
        struct stat st = {.st_ino = ino, .st_mode = S_IFDIR};
        size_t entsize = fuse_add_direntry(req, buf + buf_used, size - buf_used,
                                           ".", &st, ++entry_idx);
        if (entsize > size - buf_used) goto done;
        buf_used += entsize;
    }

    if (off <= 1) {
        struct stat st = {.st_ino = ino, .st_mode = S_IFDIR};  /* parent, simplified */
        size_t entsize = fuse_add_direntry(req, buf + buf_used, size - buf_used,
                                           "..", &st, ++entry_idx);
        if (entsize > size - buf_used) goto done;
        buf_used += entsize;
    }

    /* 遍历目录项 */
    char prefix[64];
    int prefix_len = kvbfs_key_dirent_prefix(prefix, sizeof(prefix), ino);

    kv_iterator_t *iter = kv_iter_prefix(g_ctx->db, prefix, prefix_len);
    while (kv_iter_valid(iter)) {
        entry_idx++;

        if (entry_idx <= off) {
            kv_iter_next(iter);
            continue;
        }

        size_t key_len;
        const char *key = kv_iter_key(iter, &key_len);

        /* 提取文件名：跳过 "d:<parent>:" 前缀 */
        const char *name = key + prefix_len;
        size_t name_len = key_len - prefix_len;

        /* 获取子 inode 号 */
        size_t val_len;
        const char *val = kv_iter_value(iter, &val_len);
        uint64_t child_ino;
        memcpy(&child_ino, val, sizeof(uint64_t));

        /* 获取子 inode 属性 */
        struct kvbfs_inode child_inode;
        struct stat st = {0};
        if (inode_load(child_ino, &child_inode) == 0) {
            st.st_ino = child_ino;
            st.st_mode = child_inode.mode;
        }

        /* 需要 null-terminated name */
        char name_buf[256];
        if (name_len >= sizeof(name_buf)) name_len = sizeof(name_buf) - 1;
        memcpy(name_buf, name, name_len);
        name_buf[name_len] = '\0';

        size_t entsize = fuse_add_direntry(req, buf + buf_used, size - buf_used,
                                           name_buf, &st, entry_idx + 1);
        if (entsize > size - buf_used) {
            kv_iter_free(iter);
            goto done;
        }
        buf_used += entsize;

        kv_iter_next(iter);
    }
    kv_iter_free(iter);

#ifdef CFS_MEMORY
    /* Append virtual .agentfs and .events entries when listing root */
    if (ino == KVBFS_ROOT_INO) {
        entry_idx++;
        if (entry_idx > off) {
            struct stat ctl_st;
            agentfs_ctl_stat(&ctl_st);
            size_t entsize = fuse_add_direntry(req, buf + buf_used, size - buf_used,
                                               AGENTFS_CTL_NAME, &ctl_st, entry_idx + 1);
            if (entsize <= size - buf_used)
                buf_used += entsize;
        }
        entry_idx++;
        if (entry_idx > off) {
            struct stat evt_st;
            agentfs_events_stat(&evt_st);
            size_t entsize = fuse_add_direntry(req, buf + buf_used, size - buf_used,
                                               AGENTFS_EVENTS_NAME, &evt_st, entry_idx + 1);
            if (entsize <= size - buf_used)
                buf_used += entsize;
        }
        entry_idx++;
        if (entry_idx > off) {
            struct stat ver_st;
            versions_root_stat(&ver_st);
            size_t entsize = fuse_add_direntry(req, buf + buf_used, size - buf_used,
                                               AGENTFS_VERSIONS_NAME, &ver_st, entry_idx + 1);
            if (entsize <= size - buf_used)
                buf_used += entsize;
        }
    }
#endif

done:
    fuse_reply_buf(req, buf, buf_used);
    free(buf);
}

static void kvbfs_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode)
{
    /* 检查父目录存在且是目录 */
    struct kvbfs_inode_cache *pic = inode_get(parent);
    if (!pic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_rdlock(&pic->lock);
    int is_dir = S_ISDIR(pic->inode.mode);
    pthread_rwlock_unlock(&pic->lock);

    if (!is_dir) {
        inode_put(pic);
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    /* 检查是否已存在 */
    if (dirent_lookup(parent, name) != 0) {
        inode_put(pic);
        fuse_reply_err(req, EEXIST);
        return;
    }

    /* 创建新目录 inode */
    struct kvbfs_inode_cache *ic = inode_create(S_IFDIR | (mode & 0777));
    if (!ic) {
        inode_put(pic);
        fuse_reply_err(req, EIO);
        return;
    }

    /* 设置 nlink = 2 (. 和 父目录的引用) */
    pthread_rwlock_wrlock(&ic->lock);
    ic->inode.nlink = 2;
    pthread_rwlock_unlock(&ic->lock);
    inode_sync(ic);

    /* 添加目录项 */
    if (dirent_add(parent, name, ic->inode.ino) != 0) {
        inode_delete(ic->inode.ino);
        inode_put(ic);
        inode_put(pic);
        fuse_reply_err(req, EIO);
        return;
    }

    /* 增加父目录 nlink */
    pthread_rwlock_wrlock(&pic->lock);
    pic->inode.nlink++;
    pthread_rwlock_unlock(&pic->lock);
    inode_sync(pic);
    inode_put(pic);

    /* 返回新目录信息 */
    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.ino = ic->inode.ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;

    pthread_rwlock_rdlock(&ic->lock);
    inode_to_stat(&ic->inode, &e.attr);
    pthread_rwlock_unlock(&ic->lock);

    inode_put(ic);

    fuse_reply_entry(req, &e);

#ifdef CFS_MEMORY
    events_emit(&g_ctx->events, EVT_MKDIR, e.ino, name);
#endif
}

static void kvbfs_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    /* 查找目标目录 */
    uint64_t child_ino = dirent_lookup(parent, name);
    if (child_ino == 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    struct kvbfs_inode_cache *ic = inode_get(child_ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_rdlock(&ic->lock);
    int is_dir = S_ISDIR(ic->inode.mode);
    pthread_rwlock_unlock(&ic->lock);

    if (!is_dir) {
        inode_put(ic);
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    /* 检查目录是否为空 */
    if (!dirent_is_empty(child_ino)) {
        inode_put(ic);
        fuse_reply_err(req, ENOTEMPTY);
        return;
    }

    /* 删除目录项 */
    if (dirent_remove(parent, name) != 0) {
        inode_put(ic);
        fuse_reply_err(req, EIO);
        return;
    }

    /* 减少父目录 nlink */
    struct kvbfs_inode_cache *pic = inode_get(parent);
    if (pic) {
        pthread_rwlock_wrlock(&pic->lock);
        if (pic->inode.nlink > 0) pic->inode.nlink--;
        pthread_rwlock_unlock(&pic->lock);
        inode_sync(pic);
        inode_put(pic);
    }

    /* 删除 inode */
    inode_put(ic);
    xattr_delete_all(child_ino);
    version_delete_all(child_ino);
#ifdef CFS_MEMORY
    mem_delete_embeddings(g_ctx->db, child_ino);
#endif
    inode_delete(child_ino);

#ifdef CFS_MEMORY
    events_emit(&g_ctx->events, EVT_RMDIR, child_ino, name);
#endif
    fuse_reply_err(req, 0);
}

static void kvbfs_create(fuse_req_t req, fuse_ino_t parent, const char *name,
                         mode_t mode, struct fuse_file_info *fi)
{
    /* 检查父目录 */
    struct kvbfs_inode_cache *pic = inode_get(parent);
    if (!pic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_rdlock(&pic->lock);
    int is_dir = S_ISDIR(pic->inode.mode);
    pthread_rwlock_unlock(&pic->lock);
    inode_put(pic);

    if (!is_dir) {
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    /* 检查是否已存在 */
    if (dirent_lookup(parent, name) != 0) {
        fuse_reply_err(req, EEXIST);
        return;
    }

    /* 创建新文件 inode */
    struct kvbfs_inode_cache *ic = inode_create(S_IFREG | (mode & 0777));
    if (!ic) {
        fuse_reply_err(req, EIO);
        return;
    }

    /* 添加目录项 */
    if (dirent_add(parent, name, ic->inode.ino) != 0) {
        inode_delete(ic->inode.ino);
        inode_put(ic);
        fuse_reply_err(req, EIO);
        return;
    }

#ifdef CFS_LOCAL_LLM
    /* Track new session files in O(1) hash set */
    if (parent == g_ctx->sessions_ino) {
        struct session_ino_entry *entry = malloc(sizeof(*entry));
        if (entry) {
            entry->ino = ic->inode.ino;
            pthread_mutex_lock(&g_ctx->session_lock);
            HASH_ADD(hh, g_ctx->session_set, ino, sizeof(uint64_t), entry);
            pthread_mutex_unlock(&g_ctx->session_lock);
        }
    }
#endif

    /* 返回新文件信息 */
    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.ino = ic->inode.ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;

    pthread_rwlock_rdlock(&ic->lock);
    inode_to_stat(&ic->inode, &e.attr);
    pthread_rwlock_unlock(&ic->lock);

    /* Allocate per-open file handle for write tracking */
    {
        struct kvbfs_fh *fh = calloc(1, sizeof(struct kvbfs_fh));
        if (fh) {
            fh->ino = ic->inode.ino;
            fh->written = false;
        }
        fi->fh = (uint64_t)(uintptr_t)fh;
    }

    inode_put(ic);

    fuse_reply_create(req, &e, fi);

#ifdef CFS_MEMORY
    events_emit(&g_ctx->events, EVT_CREATE, e.ino, name);
#endif
}

static void kvbfs_unlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    /* 查找文件 */
    uint64_t child_ino = dirent_lookup(parent, name);
    if (child_ino == 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    struct kvbfs_inode_cache *ic = inode_get(child_ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_rdlock(&ic->lock);
    int is_dir = S_ISDIR(ic->inode.mode);
    uint64_t blocks = ic->inode.blocks;
    pthread_rwlock_unlock(&ic->lock);

    if (is_dir) {
        inode_put(ic);
        fuse_reply_err(req, EISDIR);
        return;
    }

    /* 删除目录项 */
    if (dirent_remove(parent, name) != 0) {
        inode_put(ic);
        fuse_reply_err(req, EIO);
        return;
    }

    /* 减少 nlink，如果为 0 则删除 */
    pthread_rwlock_wrlock(&ic->lock);
    if (ic->inode.nlink > 0) ic->inode.nlink--;
    int should_delete = (ic->inode.nlink == 0);
    pthread_rwlock_unlock(&ic->lock);

    inode_put(ic);

    if (should_delete) {
        delete_file_blocks(child_ino, blocks);
        xattr_delete_all(child_ino);
        version_delete_all(child_ino);
#ifdef CFS_MEMORY
        mem_delete_embeddings(g_ctx->db, child_ino);
#endif
        inode_delete(child_ino);
    }

#ifdef CFS_LOCAL_LLM
    /* Remove from session hash set if parent is /sessions */
    if (parent == g_ctx->sessions_ino) {
        uint64_t key = child_ino;
        struct session_ino_entry *entry = NULL;
        pthread_mutex_lock(&g_ctx->session_lock);
        HASH_FIND(hh, g_ctx->session_set, &key, sizeof(uint64_t), entry);
        if (entry) {
            HASH_DEL(g_ctx->session_set, entry);
            free(entry);
        }
        pthread_mutex_unlock(&g_ctx->session_lock);
    }
#endif

#ifdef CFS_MEMORY
    events_emit(&g_ctx->events, EVT_UNLINK, child_ino, name);
#endif
    fuse_reply_err(req, 0);
}

static void kvbfs_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
#ifdef CFS_MEMORY
    if (ino == AGENTFS_CTL_INO) {
        struct agentfs_ctl_fh *ctl = calloc(1, sizeof(struct agentfs_ctl_fh));
        if (!ctl) {
            fuse_reply_err(req, ENOMEM);
            return;
        }
        fi->fh = (uint64_t)(uintptr_t)ctl;
        fi->direct_io = 1;  /* bypass page cache for dynamic content */
        fuse_reply_open(req, fi);
        return;
    }
    if (ino == AGENTFS_EVENTS_INO) {
        /* Read-only check */
        if ((fi->flags & O_ACCMODE) != O_RDONLY) {
            fuse_reply_err(req, EACCES);
            return;
        }
        struct events_fh *efh = calloc(1, sizeof(struct events_fh));
        if (!efh) {
            fuse_reply_err(req, ENOMEM);
            return;
        }
        pthread_mutex_lock(&g_ctx->events.lock);
        efh->start_seq = g_ctx->events.seq;
        efh->read_pos = g_ctx->events.head;
        pthread_mutex_unlock(&g_ctx->events.lock);
        fi->fh = (uint64_t)(uintptr_t)efh;
        fi->direct_io = 1;
        fuse_reply_open(req, fi);
        return;
    }
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
#endif

    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_rdlock(&ic->lock);
    int is_file = S_ISREG(ic->inode.mode);
    pthread_rwlock_unlock(&ic->lock);

    if (!is_file) {
        inode_put(ic);
        fuse_reply_err(req, EISDIR);
        return;
    }

    /* 处理 O_TRUNC：截断文件为 0 */
    if (fi->flags & O_TRUNC) {
        pthread_rwlock_wrlock(&ic->lock);
        uint64_t old_blocks = ic->inode.blocks;
        if (old_blocks > 0) {
            delete_file_blocks(ino, old_blocks);
        }
        ic->inode.size = 0;
        ic->inode.blocks = 0;
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        ic->inode.mtime = now;
        ic->inode.ctime = now;
        pthread_rwlock_unlock(&ic->lock);
        inode_sync(ic);
    }

    inode_put(ic);

    /* Allocate per-open file handle for write tracking */
    struct kvbfs_fh *fh = calloc(1, sizeof(struct kvbfs_fh));
    if (fh) {
        fh->ino = ino;
        fh->written = (fi->flags & O_TRUNC) ? true : false;
    }
    fi->fh = (uint64_t)(uintptr_t)fh;

    fuse_reply_open(req, fi);
}

static void kvbfs_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
#ifdef CFS_MEMORY
    if (ino == AGENTFS_CTL_INO) {
        struct agentfs_ctl_fh *ctl = (struct agentfs_ctl_fh *)(uintptr_t)fi->fh;
        if (ctl) {
            free(ctl->query);
            free(ctl->result);
            free(ctl);
        }
        fuse_reply_err(req, 0);
        return;
    }
    if (ino == AGENTFS_EVENTS_INO) {
        struct events_fh *efh = (struct events_fh *)(uintptr_t)fi->fh;
        free(efh);
        fuse_reply_err(req, 0);
        return;
    }
    if (vtree_is_vnode(ino)) {
        struct version_fh *vfh = (struct version_fh *)(uintptr_t)fi->fh;
        free(vfh);
        fuse_reply_err(req, 0);
        return;
    }
#endif

    struct kvbfs_fh *fh = (struct kvbfs_fh *)(uintptr_t)fi->fh;

#ifdef CFS_LOCAL_LLM
    /* 如果文件在 /sessions 目录下且可写打开，尝试触发推理 */
    if (fi->flags != O_RDONLY && is_session_file(ino))
        llm_submit(&g_ctx->llm, ino);
#else
    (void)ino;
#endif

    if (fh && fh->written) {
        version_snapshot(fh->ino);
#ifdef CFS_MEMORY
        mem_index_file(&g_ctx->mem, g_ctx->db, fh->ino);
        events_emit(&g_ctx->events, EVT_WRITE, fh->ino, NULL);
#endif
    }

    free(fh);
    fuse_reply_err(req, 0);
}

static void kvbfs_read(fuse_req_t req, fuse_ino_t ino, size_t size,
                       off_t off, struct fuse_file_info *fi)
{
#ifdef CFS_MEMORY
    if (ino == AGENTFS_CTL_INO) {
        struct agentfs_ctl_fh *ctl = (struct agentfs_ctl_fh *)(uintptr_t)fi->fh;
        if (!ctl) {
            fuse_reply_err(req, EIO);
            return;
        }

        /* Execute search on first read if results not yet generated */
        if (!ctl->result) {
            if (agentfs_ctl_search(ctl) != 0) {
                fuse_reply_err(req, EIO);
                return;
            }
        }

        /* Return data from result buffer at offset */
        if ((size_t)off >= ctl->result_len) {
            fuse_reply_buf(req, NULL, 0);
        } else {
            size_t avail = ctl->result_len - off;
            if (size > avail) size = avail;
            fuse_reply_buf(req, ctl->result + off, size);
        }
        return;
    }
    if (ino == AGENTFS_EVENTS_INO) {
        struct events_fh *efh = (struct events_fh *)(uintptr_t)fi->fh;
        if (!efh) {
            fuse_reply_err(req, EIO);
            return;
        }

        struct events_ctx *ectx = &g_ctx->events;
        pthread_mutex_lock(&ectx->lock);

        /* If read_pos has been overwritten by ring wraparound, advance to tail */
        if (efh->read_pos < ectx->tail)
            efh->read_pos = ectx->tail;

        size_t avail = ectx->head - efh->read_pos;
        if (avail == 0) {
            pthread_mutex_unlock(&ectx->lock);
            fuse_reply_buf(req, NULL, 0);
            return;
        }

        if (size > avail) size = avail;

        /* Copy from ring buffer (may wrap around) */
        char *tmp = malloc(size);
        if (!tmp) {
            pthread_mutex_unlock(&ectx->lock);
            fuse_reply_err(req, ENOMEM);
            return;
        }
        for (size_t i = 0; i < size; i++)
            tmp[i] = ectx->ring[(efh->read_pos + i) % EVENTS_RING_SIZE];
        efh->read_pos += size;

        pthread_mutex_unlock(&ectx->lock);

        fuse_reply_buf(req, tmp, size);
        free(tmp);
        return;
    }
    if (vtree_is_vnode(ino)) {
        struct version_fh *vfh = (struct version_fh *)(uintptr_t)fi->fh;
        if (!vfh) { fuse_reply_err(req, EIO); return; }

        uint64_t start_block = (uint64_t)off / KVBFS_BLOCK_SIZE;
        uint64_t end_block   = ((uint64_t)off + size - 1) / KVBFS_BLOCK_SIZE;

        char *outbuf = calloc(1, size);
        if (!outbuf) { fuse_reply_err(req, ENOMEM); return; }
        size_t copied = 0;

        for (uint64_t blk = start_block; blk <= end_block && copied < size; blk++) {
            char *data = NULL;
            size_t dlen = 0;
            if (version_read_block(vfh->real_ino, vfh->version, blk,
                                   &data, &dlen) != 0)
                break;
            size_t blk_off = (blk == start_block) ? ((uint64_t)off % KVBFS_BLOCK_SIZE) : 0;
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
#endif

    (void)fi;

    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_rdlock(&ic->lock);
    uint64_t file_size = ic->inode.size;
    pthread_rwlock_unlock(&ic->lock);

    inode_put(ic);

    /* 超出文件末尾 */
    if ((uint64_t)off >= file_size) {
        fuse_reply_buf(req, NULL, 0);
        return;
    }

    /* 调整读取大小 */
    if (off + size > file_size) {
        size = file_size - off;
    }

    char *buf = malloc(size);
    if (!buf) {
        fuse_reply_err(req, ENOMEM);
        return;
    }

    size_t bytes_read = 0;
    uint64_t block_idx = off / KVBFS_BLOCK_SIZE;
    size_t block_off = off % KVBFS_BLOCK_SIZE;

    while (bytes_read < size) {
        char key[64];
        int keylen = kvbfs_key_block(key, sizeof(key), ino, block_idx);

        char *block_data = NULL;
        size_t block_len = 0;

        int ret = kv_get(g_ctx->db, key, keylen, &block_data, &block_len);
        if (ret != 0 || block_len <= block_off) {
            /* 块不存在或偏移超出块数据，填充零 */
            if (block_data) free(block_data);
            size_t to_copy = KVBFS_BLOCK_SIZE - block_off;
            if (to_copy > size - bytes_read) to_copy = size - bytes_read;
            memset(buf + bytes_read, 0, to_copy);
            bytes_read += to_copy;
        } else {
            size_t to_copy = block_len - block_off;
            if (to_copy > size - bytes_read) to_copy = size - bytes_read;
            memcpy(buf + bytes_read, block_data + block_off, to_copy);
            bytes_read += to_copy;
            free(block_data);
        }

        block_idx++;
        block_off = 0;  /* 后续块从头开始 */
    }

    fuse_reply_buf(req, buf, bytes_read);
    free(buf);
}

static void kvbfs_write(fuse_req_t req, fuse_ino_t ino, const char *buf,
                        size_t size, off_t off, struct fuse_file_info *fi)
{
#ifdef CFS_MEMORY
    if (ino == AGENTFS_EVENTS_INO) {
        fuse_reply_err(req, EACCES);
        return;
    }
    if (ino == AGENTFS_CTL_INO) {
        struct agentfs_ctl_fh *ctl = (struct agentfs_ctl_fh *)(uintptr_t)fi->fh;
        if (!ctl) {
            fuse_reply_err(req, EIO);
            return;
        }
        /* Accumulate query data; clear any previous results */
        free(ctl->result);
        ctl->result = NULL;
        ctl->result_len = 0;

        char *newq = realloc(ctl->query, ctl->query_len + size);
        if (!newq) {
            fuse_reply_err(req, ENOMEM);
            return;
        }
        memcpy(newq + ctl->query_len, buf, size);
        ctl->query = newq;
        ctl->query_len += size;
        (void)off;

        fuse_reply_write(req, size);
        return;
    }
#endif

    /* Mark file handle as written */
    struct kvbfs_fh *fh = (struct kvbfs_fh *)(uintptr_t)fi->fh;
    if (fh) fh->written = true;

    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    size_t bytes_written = 0;
    uint64_t block_idx = off / KVBFS_BLOCK_SIZE;
    size_t block_off = off % KVBFS_BLOCK_SIZE;

    while (bytes_written < size) {
        char key[64];
        int keylen = kvbfs_key_block(key, sizeof(key), ino, block_idx);

        /* 读取现有块（如果存在）或创建新块 */
        char block[KVBFS_BLOCK_SIZE];
        memset(block, 0, KVBFS_BLOCK_SIZE);

        char *existing = NULL;
        size_t existing_len = 0;
        if (kv_get(g_ctx->db, key, keylen, &existing, &existing_len) == 0) {
            size_t copy_len = existing_len < KVBFS_BLOCK_SIZE ? existing_len : KVBFS_BLOCK_SIZE;
            memcpy(block, existing, copy_len);
            free(existing);
        }

        /* 写入数据到块 */
        size_t to_write = KVBFS_BLOCK_SIZE - block_off;
        if (to_write > size - bytes_written) to_write = size - bytes_written;

        memcpy(block + block_off, buf + bytes_written, to_write);

        /* 保存完整块，保持统一块大小 */
        if (kv_put(g_ctx->db, key, keylen, block, KVBFS_BLOCK_SIZE) != 0) {
            inode_put(ic);
            fuse_reply_err(req, EIO);
            return;
        }

        bytes_written += to_write;
        block_idx++;
        block_off = 0;
    }

    /* 更新文件大小 */
    pthread_rwlock_wrlock(&ic->lock);
    if ((uint64_t)(off + size) > ic->inode.size) {
        ic->inode.size = off + size;
    }
    ic->inode.blocks = (ic->inode.size + KVBFS_BLOCK_SIZE - 1) / KVBFS_BLOCK_SIZE;

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    ic->inode.mtime = now;
    ic->inode.ctime = now;
    pthread_rwlock_unlock(&ic->lock);

    inode_sync(ic);
    inode_put(ic);

    fuse_reply_write(req, bytes_written);
}

static void kvbfs_rename(fuse_req_t req, fuse_ino_t parent, const char *name,
                         fuse_ino_t newparent, const char *newname, unsigned int flags)
{
    (void)flags;  /* 暂不支持 RENAME_EXCHANGE 等 */

    /* 查找源文件 */
    uint64_t src_ino = dirent_lookup(parent, name);
    if (src_ino == 0) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    /* 检查目标是否已存在 */
    uint64_t dst_ino = dirent_lookup(newparent, newname);
    if (dst_ino != 0) {
        /* 目标存在，需要先删除 */
        struct kvbfs_inode_cache *dst_ic = inode_get(dst_ino);
        if (dst_ic) {
            pthread_rwlock_rdlock(&dst_ic->lock);
            int is_dir = S_ISDIR(dst_ic->inode.mode);
            uint64_t blocks = dst_ic->inode.blocks;
            pthread_rwlock_unlock(&dst_ic->lock);
            inode_put(dst_ic);

            if (is_dir) {
                if (!dirent_is_empty(dst_ino)) {
                    fuse_reply_err(req, ENOTEMPTY);
                    return;
                }
            }

            dirent_remove(newparent, newname);
            if (!is_dir) {
                delete_file_blocks(dst_ino, blocks);
            } else {
                /* 被替换的目标是目录，减少 newparent 的 nlink */
                struct kvbfs_inode_cache *np_ic = inode_get(newparent);
                if (np_ic) {
                    pthread_rwlock_wrlock(&np_ic->lock);
                    if (np_ic->inode.nlink > 0) np_ic->inode.nlink--;
                    pthread_rwlock_unlock(&np_ic->lock);
                    inode_sync(np_ic);
                    inode_put(np_ic);
                }
            }
            xattr_delete_all(dst_ino);
            version_delete_all(dst_ino);
#ifdef CFS_MEMORY
            mem_delete_embeddings(g_ctx->db, dst_ino);
#endif
            inode_delete(dst_ino);
        }
    }

    /* 获取源 inode 信息 */
    struct kvbfs_inode_cache *src_ic = inode_get(src_ino);
    int src_is_dir = 0;
    if (src_ic) {
        pthread_rwlock_rdlock(&src_ic->lock);
        src_is_dir = S_ISDIR(src_ic->inode.mode);
        pthread_rwlock_unlock(&src_ic->lock);
        inode_put(src_ic);
    }

    /* 删除旧目录项 */
    if (dirent_remove(parent, name) != 0) {
        fuse_reply_err(req, EIO);
        return;
    }

    /* 添加新目录项 */
    if (dirent_add(newparent, newname, src_ino) != 0) {
        /* 尝试恢复 */
        dirent_add(parent, name, src_ino);
        fuse_reply_err(req, EIO);
        return;
    }

#ifdef CFS_LOCAL_LLM
    /* Maintain session hash set on rename across /sessions boundary */
    if (parent != newparent) {
        if (parent == g_ctx->sessions_ino) {
            /* Moved out of /sessions: remove from set */
            uint64_t key = src_ino;
            struct session_ino_entry *entry = NULL;
            pthread_mutex_lock(&g_ctx->session_lock);
            HASH_FIND(hh, g_ctx->session_set, &key, sizeof(uint64_t), entry);
            if (entry) {
                HASH_DEL(g_ctx->session_set, entry);
                free(entry);
            }
            pthread_mutex_unlock(&g_ctx->session_lock);
        }
        if (newparent == g_ctx->sessions_ino) {
            /* Moved into /sessions: add to set */
            struct session_ino_entry *entry = malloc(sizeof(*entry));
            if (entry) {
                entry->ino = src_ino;
                pthread_mutex_lock(&g_ctx->session_lock);
                HASH_ADD(hh, g_ctx->session_set, ino, sizeof(uint64_t), entry);
                pthread_mutex_unlock(&g_ctx->session_lock);
            }
        }
    }
#endif

    /* 如果是目录且跨目录移动，更新父目录 nlink */
    if (src_is_dir && parent != newparent) {
        struct kvbfs_inode_cache *old_pic = inode_get(parent);
        if (old_pic) {
            pthread_rwlock_wrlock(&old_pic->lock);
            if (old_pic->inode.nlink > 0) old_pic->inode.nlink--;
            pthread_rwlock_unlock(&old_pic->lock);
            inode_sync(old_pic);
            inode_put(old_pic);
        }

        struct kvbfs_inode_cache *new_pic = inode_get(newparent);
        if (new_pic) {
            pthread_rwlock_wrlock(&new_pic->lock);
            new_pic->inode.nlink++;
            pthread_rwlock_unlock(&new_pic->lock);
            inode_sync(new_pic);
            inode_put(new_pic);
        }
    }

#ifdef CFS_MEMORY
    events_emit(&g_ctx->events, EVT_RENAME, src_ino, newname);
#endif
    fuse_reply_err(req, 0);
}

static void kvbfs_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
                        struct fuse_file_info *fi)
{
    (void)datasync;
    (void)fi;

    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    int ret = inode_sync(ic);
    inode_put(ic);

    fuse_reply_err(req, ret == 0 ? 0 : EIO);
}

static void kvbfs_symlink(fuse_req_t req, const char *link, fuse_ino_t parent,
                          const char *name)
{
    /* 检查父目录 */
    struct kvbfs_inode_cache *pic = inode_get(parent);
    if (!pic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_rdlock(&pic->lock);
    int is_dir = S_ISDIR(pic->inode.mode);
    pthread_rwlock_unlock(&pic->lock);
    inode_put(pic);

    if (!is_dir) {
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    /* 检查是否已存在 */
    if (dirent_lookup(parent, name) != 0) {
        fuse_reply_err(req, EEXIST);
        return;
    }

    /* 创建 symlink inode */
    struct kvbfs_inode_cache *ic = inode_create(S_IFLNK | 0777);
    if (!ic) {
        fuse_reply_err(req, EIO);
        return;
    }

    uint64_t ino = ic->inode.ino;

    /* 将 symlink 目标存储为 block 0 */
    size_t link_len = strlen(link);
    char key[64];
    int keylen = kvbfs_key_block(key, sizeof(key), ino, 0);
    if (kv_put(g_ctx->db, key, keylen, link, link_len) != 0) {
        inode_delete(ino);
        inode_put(ic);
        fuse_reply_err(req, EIO);
        return;
    }

    /* 更新 inode 大小和块数 */
    pthread_rwlock_wrlock(&ic->lock);
    ic->inode.size = link_len;
    ic->inode.blocks = 1;
    pthread_rwlock_unlock(&ic->lock);
    inode_sync(ic);

    /* 添加目录项 */
    if (dirent_add(parent, name, ino) != 0) {
        char bkey[64];
        int bkeylen = kvbfs_key_block(bkey, sizeof(bkey), ino, 0);
        kv_delete(g_ctx->db, bkey, bkeylen);
        inode_delete(ino);
        inode_put(ic);
        fuse_reply_err(req, EIO);
        return;
    }

    /* 返回新 symlink 信息 */
    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.ino = ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;

    pthread_rwlock_rdlock(&ic->lock);
    inode_to_stat(&ic->inode, &e.attr);
    pthread_rwlock_unlock(&ic->lock);

    inode_put(ic);

    fuse_reply_entry(req, &e);
}

static void kvbfs_readlink(fuse_req_t req, fuse_ino_t ino)
{
    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_rdlock(&ic->lock);
    int is_link = S_ISLNK(ic->inode.mode);
    pthread_rwlock_unlock(&ic->lock);

    inode_put(ic);

    if (!is_link) {
        fuse_reply_err(req, EINVAL);
        return;
    }

    /* 从 block 0 读取 symlink 目标 */
    char key[64];
    int keylen = kvbfs_key_block(key, sizeof(key), ino, 0);

    char *target = NULL;
    size_t target_len = 0;
    if (kv_get(g_ctx->db, key, keylen, &target, &target_len) != 0) {
        fuse_reply_err(req, EIO);
        return;
    }

    /* null-terminate */
    char *buf = malloc(target_len + 1);
    if (!buf) {
        free(target);
        fuse_reply_err(req, ENOMEM);
        return;
    }
    memcpy(buf, target, target_len);
    buf[target_len] = '\0';
    free(target);

    fuse_reply_readlink(req, buf);
    free(buf);
}

static void kvbfs_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
                       const char *newname)
{
    /* 获取源 inode */
    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_rdlock(&ic->lock);
    int is_dir = S_ISDIR(ic->inode.mode);
    pthread_rwlock_unlock(&ic->lock);

    if (is_dir) {
        inode_put(ic);
        fuse_reply_err(req, EPERM);
        return;
    }

    /* 检查目标父目录 */
    struct kvbfs_inode_cache *pic = inode_get(newparent);
    if (!pic) {
        inode_put(ic);
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_rdlock(&pic->lock);
    int parent_is_dir = S_ISDIR(pic->inode.mode);
    pthread_rwlock_unlock(&pic->lock);
    inode_put(pic);

    if (!parent_is_dir) {
        inode_put(ic);
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    /* 检查目标名是否已存在 */
    if (dirent_lookup(newparent, newname) != 0) {
        inode_put(ic);
        fuse_reply_err(req, EEXIST);
        return;
    }

    /* 添加目录项 */
    if (dirent_add(newparent, newname, ino) != 0) {
        inode_put(ic);
        fuse_reply_err(req, EIO);
        return;
    }

    /* 增加 nlink，更新 ctime */
    pthread_rwlock_wrlock(&ic->lock);
    ic->inode.nlink++;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    ic->inode.ctime = now;

    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.ino = ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;
    inode_to_stat(&ic->inode, &e.attr);

    pthread_rwlock_unlock(&ic->lock);

    inode_sync(ic);
    inode_put(ic);

    fuse_reply_entry(req, &e);

#ifdef CFS_MEMORY
    events_emit(&g_ctx->events, EVT_LINK, ino, newname);
#endif
}

#ifdef CFS_LOCAL_LLM
/* 检查 ino 是否是 /sessions 目录下的文件 (O(1) hash lookup) */
static int is_session_file(fuse_ino_t ino)
{
    if (g_ctx->sessions_ino == 0) return 0;

    uint64_t key = (uint64_t)ino;
    struct session_ino_entry *entry = NULL;

    pthread_mutex_lock(&g_ctx->session_lock);
    HASH_FIND(hh, g_ctx->session_set, &key, sizeof(uint64_t), entry);
    pthread_mutex_unlock(&g_ctx->session_lock);

    return entry != NULL;
}
#endif

static void kvbfs_poll_op(fuse_req_t req, fuse_ino_t ino,
                          struct fuse_file_info *fi, struct fuse_pollhandle *ph)
{
    (void)fi;
    (void)ino;

#ifdef CFS_MEMORY
    if (ino == AGENTFS_EVENTS_INO) {
        struct events_fh *efh = (struct events_fh *)(uintptr_t)fi->fh;
        struct events_ctx *ectx = &g_ctx->events;
        pthread_mutex_lock(&ectx->lock);

        int has_data = (efh && efh->read_pos < ectx->head);

        if (has_data) {
            if (ph) fuse_pollhandle_destroy(ph);
            pthread_mutex_unlock(&ectx->lock);
            fuse_reply_poll(req, POLLIN);
        } else {
            /* Register for notification */
            if (ph) {
                if (ectx->ph) fuse_pollhandle_destroy(ectx->ph);
                ectx->ph = ph;
            }
            pthread_mutex_unlock(&ectx->lock);
            fuse_reply_poll(req, 0);
        }
        return;
    }
#endif

#ifdef CFS_LOCAL_LLM
    if (is_session_file(ino) && llm_gen_is_active(&g_ctx->llm, ino)) {
        /* 正在生成中：注册 waiter，不报告就绪 */
        if (ph)
            llm_gen_add_waiter(&g_ctx->llm, ino, ph);
        fuse_reply_poll(req, 0);
        return;
    }
#endif

    /* 普通文件或未在生成：立即就绪 */
    if (ph)
        fuse_pollhandle_destroy(ph);
    fuse_reply_poll(req, POLLIN | POLLOUT);
}

static void kvbfs_ioctl_op(fuse_req_t req, fuse_ino_t ino, unsigned int cmd,
                           void *arg, struct fuse_file_info *fi,
                           unsigned flags, const void *in_buf, size_t in_bufsz,
                           size_t out_bufsz)
{
    (void)arg;
    (void)fi;

#if defined(CFS_LOCAL_LLM) || defined(CFS_MEMORY)
    if (flags & FUSE_IOCTL_COMPAT) {
        fuse_reply_err(req, ENOSYS);
        return;
    }

    switch (cmd) {
#ifdef CFS_LOCAL_LLM
    case CFS_IOC_STATUS: {
        if (out_bufsz < sizeof(struct cfs_status)) {
            struct iovec iov = { .iov_base = NULL, .iov_len = 0 };
            struct iovec out_iov = { .iov_base = NULL,
                                     .iov_len = sizeof(struct cfs_status) };
            fuse_reply_ioctl_retry(req, &iov, 0, &out_iov, 1);
            return;
        }

        struct cfs_status st = {0};
        st.generating = llm_gen_is_active(&g_ctx->llm, ino) ? 1 : 0;
        fuse_reply_ioctl(req, 0, &st, sizeof(st));
        return;
    }
    case CFS_IOC_CANCEL:
        fuse_reply_err(req, ENOSYS);
        return;
#endif /* CFS_LOCAL_LLM */
#ifdef CFS_MEMORY
    case CFS_IOC_MEM_SEARCH: {
        if (in_bufsz < sizeof(struct cfs_mem_query)) {
            struct iovec in_iov = { .iov_base = NULL,
                                    .iov_len = sizeof(struct cfs_mem_query) };
            struct iovec out_iov = { .iov_base = NULL,
                                     .iov_len = sizeof(struct cfs_mem_query) };
            fuse_reply_ioctl_retry(req, &in_iov, 1, &out_iov, 1);
            return;
        }
        if (out_bufsz < sizeof(struct cfs_mem_query)) {
            struct iovec in_iov = { .iov_base = NULL,
                                    .iov_len = sizeof(struct cfs_mem_query) };
            struct iovec out_iov = { .iov_base = NULL,
                                     .iov_len = sizeof(struct cfs_mem_query) };
            fuse_reply_ioctl_retry(req, &in_iov, 1, &out_iov, 1);
            return;
        }

        struct cfs_mem_query query;
        memcpy(&query, in_buf, sizeof(query));
        query.query_text[sizeof(query.query_text) - 1] = '\0';

        int ret = mem_search(&g_ctx->mem, g_ctx->db, &query);
        if (ret != 0) {
            fuse_reply_err(req, EIO);
            return;
        }

        fuse_reply_ioctl(req, 0, &query, sizeof(query));
        return;
    }
#endif /* CFS_MEMORY */
    default:
        break;
    }
#else
    (void)ino;
    (void)cmd;
    (void)flags;
    (void)in_buf;
    (void)in_bufsz;
    (void)out_bufsz;
#endif

    fuse_reply_err(req, ENOTTY);
}

/* ---- xattr operations ---- */

static void kvbfs_setxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                            const char *value, size_t size, int flags)
{
#ifdef CFS_MEMORY
    if (ino == AGENTFS_CTL_INO || ino == AGENTFS_EVENTS_INO) {
        fuse_reply_err(req, ENOTSUP); return;
    }
#endif
    /* Reject writes to virtual agentfs.* namespace */
    if (strncmp(name, "agentfs.", 8) == 0) {
        fuse_reply_err(req, EPERM);
        return;
    }

    char key[KVBFS_KEY_MAX];
    int keylen = kvbfs_key_xattr(key, sizeof(key), ino, name);
    if (keylen < 0) {
        fuse_reply_err(req, ERANGE);
        return;
    }

    if (flags & XATTR_CREATE) {
        /* Must not already exist */
        char *existing = NULL;
        size_t elen = 0;
        if (kv_get(g_ctx->db, key, keylen, &existing, &elen) == 0) {
            free(existing);
            fuse_reply_err(req, EEXIST);
            return;
        }
    } else if (flags & XATTR_REPLACE) {
        /* Must already exist */
        char *existing = NULL;
        size_t elen = 0;
        if (kv_get(g_ctx->db, key, keylen, &existing, &elen) != 0) {
            fuse_reply_err(req, ENODATA);
            return;
        }
        free(existing);
    }

    if (kv_put(g_ctx->db, key, keylen, value, size) != 0) {
        fuse_reply_err(req, EIO);
        return;
    }

#ifdef CFS_MEMORY
    events_emit(&g_ctx->events, EVT_SETXATTR, ino, name);
#endif
    fuse_reply_err(req, 0);
}

/* Helper: reply with a dynamically generated xattr value */
static void reply_virtual_xattr(fuse_req_t req, size_t size,
                                 const char *data, size_t dlen)
{
    if (size == 0) {
        fuse_reply_xattr(req, dlen);
    } else if (size < dlen) {
        fuse_reply_err(req, ERANGE);
    } else {
        fuse_reply_buf(req, data, dlen);
    }
}

static void kvbfs_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                            size_t size)
{
#ifdef CFS_MEMORY
    if (ino == AGENTFS_CTL_INO || ino == AGENTFS_EVENTS_INO) {
        fuse_reply_err(req, ENOTSUP); return;
    }
#endif
    /* Virtual xattr: agentfs.version → current version number as string */
    if (strcmp(name, "agentfs.version") == 0) {
        uint64_t ver = version_get_current(ino);
        char buf[24];
        int n = snprintf(buf, sizeof(buf), "%lu", (unsigned long)ver);
        reply_virtual_xattr(req, size, buf, n);
        return;
    }

    /* Virtual xattr: agentfs.versions → JSON array of version metadata */
    if (strcmp(name, "agentfs.versions") == 0) {
        uint64_t ver = version_get_current(ino);
        if (ver == 0) {
            reply_virtual_xattr(req, size, "[]", 2);
            return;
        }

        /* Build JSON array */
        size_t cap = 256;
        char *json = malloc(cap);
        if (!json) { fuse_reply_err(req, ENOMEM); return; }
        size_t off = 0;
        json[off++] = '[';

        uint64_t start = 0;
        if (ver > KVBFS_MAX_VERSIONS) start = ver - KVBFS_MAX_VERSIONS;

        for (uint64_t v = start; v < ver; v++) {
            struct kvbfs_version_meta meta;
            if (version_get_meta(ino, v, &meta) != 0) continue;

            char entry[128];
            int elen = snprintf(entry, sizeof(entry),
                "%s{\"ver\":%lu,\"size\":%lu,\"mtime\":%lu}",
                (off > 1) ? "," : "",
                (unsigned long)v,
                (unsigned long)meta.size,
                (unsigned long)meta.mtime.tv_sec);

            while (off + elen + 2 > cap) {
                cap *= 2;
                char *tmp = realloc(json, cap);
                if (!tmp) { free(json); fuse_reply_err(req, ENOMEM); return; }
                json = tmp;
            }
            memcpy(json + off, entry, elen);
            off += elen;
        }
        json[off++] = ']';

        reply_virtual_xattr(req, size, json, off);
        free(json);
        return;
    }

    /* Regular user xattr from KV store */
    char key[KVBFS_KEY_MAX];
    int keylen = kvbfs_key_xattr(key, sizeof(key), ino, name);
    if (keylen < 0) {
        fuse_reply_err(req, ERANGE);
        return;
    }

    char *value = NULL;
    size_t vlen = 0;
    if (kv_get(g_ctx->db, key, keylen, &value, &vlen) != 0) {
        fuse_reply_err(req, ENODATA);
        return;
    }

    if (size == 0) {
        fuse_reply_xattr(req, vlen);
    } else if (size < vlen) {
        fuse_reply_err(req, ERANGE);
    } else {
        fuse_reply_buf(req, value, vlen);
    }
    free(value);
}

static void kvbfs_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
{
#ifdef CFS_MEMORY
    if (ino == AGENTFS_CTL_INO || ino == AGENTFS_EVENTS_INO) {
        if (size == 0) fuse_reply_xattr(req, 0);
        else fuse_reply_buf(req, NULL, 0);
        return;
    }
#endif
    char prefix[64];
    int prefix_len = kvbfs_key_xattr_prefix(prefix, sizeof(prefix), ino);

    /* First pass: collect all xattr names and total size */
    size_t total = 0;
    size_t count = 0;
    size_t cap = 16;
    char **names = malloc(cap * sizeof(char *));
    size_t *name_lens = malloc(cap * sizeof(size_t));
    if (!names || !name_lens) {
        free(names);
        free(name_lens);
        fuse_reply_err(req, ENOMEM);
        return;
    }

    kv_iterator_t *iter = kv_iter_prefix(g_ctx->db, prefix, prefix_len);
    while (kv_iter_valid(iter)) {
        size_t klen;
        const char *raw_key = kv_iter_key(iter, &klen);

        /* Extract attr name: skip "x:<ino>:" prefix */
        const char *attr_name = raw_key + prefix_len;
        size_t attr_len = klen - prefix_len;

        if (count >= cap) {
            cap *= 2;
            names = realloc(names, cap * sizeof(char *));
            name_lens = realloc(name_lens, cap * sizeof(size_t));
            if (!names || !name_lens) {
                kv_iter_free(iter);
                free(names);
                free(name_lens);
                fuse_reply_err(req, ENOMEM);
                return;
            }
        }

        names[count] = strndup(attr_name, attr_len);
        name_lens[count] = attr_len;
        total += attr_len + 1;  /* name + null terminator */
        count++;

        kv_iter_next(iter);
    }
    kv_iter_free(iter);

    if (size == 0) {
        /* Return total size needed */
        fuse_reply_xattr(req, total);
    } else if (size < total) {
        fuse_reply_err(req, ERANGE);
    } else {
        /* Build null-separated name list */
        char *buf = malloc(total);
        if (!buf) {
            for (size_t i = 0; i < count; i++) free(names[i]);
            free(names);
            free(name_lens);
            fuse_reply_err(req, ENOMEM);
            return;
        }
        size_t off = 0;
        for (size_t i = 0; i < count; i++) {
            memcpy(buf + off, names[i], name_lens[i]);
            off += name_lens[i];
            buf[off++] = '\0';
        }
        fuse_reply_buf(req, buf, total);
        free(buf);
    }

    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
    free(name_lens);
}

static void kvbfs_removexattr(fuse_req_t req, fuse_ino_t ino, const char *name)
{
#ifdef CFS_MEMORY
    if (ino == AGENTFS_CTL_INO || ino == AGENTFS_EVENTS_INO) {
        fuse_reply_err(req, ENOTSUP); return;
    }
#endif
    if (strncmp(name, "agentfs.", 8) == 0) {
        fuse_reply_err(req, EPERM);
        return;
    }

    char key[KVBFS_KEY_MAX];
    int keylen = kvbfs_key_xattr(key, sizeof(key), ino, name);
    if (keylen < 0) {
        fuse_reply_err(req, ERANGE);
        return;
    }

    /* Check existence first */
    char *value = NULL;
    size_t vlen = 0;
    if (kv_get(g_ctx->db, key, keylen, &value, &vlen) != 0) {
        fuse_reply_err(req, ENODATA);
        return;
    }
    free(value);

    if (kv_delete(g_ctx->db, key, keylen) != 0) {
        fuse_reply_err(req, EIO);
        return;
    }

#ifdef CFS_MEMORY
    events_emit(&g_ctx->events, EVT_REMOVEXATTR, ino, name);
#endif
    fuse_reply_err(req, 0);
}

/* Helper: delete all xattrs for an inode */
static void xattr_delete_all(uint64_t ino)
{
    char prefix[64];
    int prefix_len = kvbfs_key_xattr_prefix(prefix, sizeof(prefix), ino);

    kv_iterator_t *iter = kv_iter_prefix(g_ctx->db, prefix, prefix_len);
    while (kv_iter_valid(iter)) {
        size_t klen;
        const char *key = kv_iter_key(iter, &klen);
        kv_delete(g_ctx->db, key, klen);
        kv_iter_next(iter);
    }
    kv_iter_free(iter);
}

/* FUSE 操作表 */
struct fuse_lowlevel_ops kvbfs_ll_ops = {
    .init       = kvbfs_init,
    .destroy    = kvbfs_destroy,
    .lookup     = kvbfs_lookup,
    .getattr    = kvbfs_getattr,
    .setattr    = kvbfs_setattr,
    .readdir    = kvbfs_readdir,
    .opendir    = kvbfs_opendir,
    .mkdir      = kvbfs_mkdir,
    .rmdir      = kvbfs_rmdir,
    .create     = kvbfs_create,
    .unlink     = kvbfs_unlink,
    .open       = kvbfs_open,
    .release    = kvbfs_release,
    .read       = kvbfs_read,
    .write      = kvbfs_write,
    .rename     = kvbfs_rename,
    .fsync      = kvbfs_fsync,
    .symlink    = kvbfs_symlink,
    .readlink   = kvbfs_readlink,
    .link       = kvbfs_link,
    .poll       = kvbfs_poll_op,
    .ioctl      = kvbfs_ioctl_op,
    .setxattr   = kvbfs_setxattr,
    .getxattr   = kvbfs_getxattr,
    .listxattr  = kvbfs_listxattr,
    .removexattr = kvbfs_removexattr,
};
