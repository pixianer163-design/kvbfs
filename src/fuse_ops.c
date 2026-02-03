#include "kvbfs.h"
#include "kv_store.h"
#include "inode.h"

#include <string.h>
#include <errno.h>
#include "super.h"
#include <unistd.h>
#include <sys/stat.h>

/* FUSE lowlevel 操作实现 */

static void inode_to_stat(const struct kvbfs_inode *inode, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino = inode->ino;
    st->st_mode = inode->mode;
    st->st_nlink = inode->nlink;
    st->st_size = inode->size;
    st->st_blocks = inode->blocks;
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
    char key[256];
    int keylen = kvbfs_key_dirent(key, sizeof(key), parent, name);

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
    char key[256];
    int keylen = kvbfs_key_dirent(key, sizeof(key), parent, name);

    return kv_put(g_ctx->db, key, keylen,
                  (const char *)&child, sizeof(uint64_t));
}

/* 删除目录项 */
static int dirent_remove(uint64_t parent, const char *name)
{
    char key[256];
    int keylen = kvbfs_key_dirent(key, sizeof(key), parent, name);

    return kv_delete(g_ctx->db, key, keylen);
}

static void kvbfs_init(void *userdata, struct fuse_conn_info *conn)
{
    (void)userdata;
    (void)conn;
    /* TODO: 初始化 */
}

static void kvbfs_destroy(void *userdata)
{
    (void)userdata;
    /* TODO: 清理 */
}

static void kvbfs_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
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
    (void)ino;
    (void)attr;
    (void)to_set;
    (void)fi;
    /* TODO: 实现 setattr */
    fuse_reply_err(req, ENOSYS);
}

static void kvbfs_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
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

done:
    fuse_reply_buf(req, buf, buf_used);
    free(buf);
}

static void kvbfs_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode)
{
    (void)parent;
    (void)name;
    (void)mode;
    /* TODO: 实现 mkdir */
    fuse_reply_err(req, ENOSYS);
}

static void kvbfs_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    (void)parent;
    (void)name;
    /* TODO: 实现 rmdir */
    fuse_reply_err(req, ENOSYS);
}

static void kvbfs_create(fuse_req_t req, fuse_ino_t parent, const char *name,
                         mode_t mode, struct fuse_file_info *fi)
{
    (void)parent;
    (void)name;
    (void)mode;
    (void)fi;
    /* TODO: 实现 create */
    fuse_reply_err(req, ENOSYS);
}

static void kvbfs_unlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    (void)parent;
    (void)name;
    /* TODO: 实现 unlink */
    fuse_reply_err(req, ENOSYS);
}

static void kvbfs_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    pthread_rwlock_rdlock(&ic->lock);
    int is_file = S_ISREG(ic->inode.mode);
    pthread_rwlock_unlock(&ic->lock);

    inode_put(ic);

    if (!is_file) {
        fuse_reply_err(req, EISDIR);
        return;
    }

    fuse_reply_open(req, fi);
}

static void kvbfs_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    (void)ino;
    (void)fi;
    /* TODO: 实现 release */
    fuse_reply_err(req, ENOSYS);
}

static void kvbfs_read(fuse_req_t req, fuse_ino_t ino, size_t size,
                       off_t off, struct fuse_file_info *fi)
{
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
        if (ret != 0) {
            /* 块不存在，填充零 */
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
    (void)ino;
    (void)buf;
    (void)size;
    (void)off;
    (void)fi;
    /* TODO: 实现 write */
    fuse_reply_err(req, ENOSYS);
}

static void kvbfs_rename(fuse_req_t req, fuse_ino_t parent, const char *name,
                         fuse_ino_t newparent, const char *newname, unsigned int flags)
{
    (void)parent;
    (void)name;
    (void)newparent;
    (void)newname;
    (void)flags;
    /* TODO: 实现 rename */
    fuse_reply_err(req, ENOSYS);
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
};
