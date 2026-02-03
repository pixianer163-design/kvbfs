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

static void kvbfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                          off_t off, struct fuse_file_info *fi)
{
    (void)ino;
    (void)size;
    (void)off;
    (void)fi;
    /* TODO: 实现 readdir */
    fuse_reply_err(req, ENOSYS);
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
    (void)ino;
    (void)fi;
    /* TODO: 实现 open */
    fuse_reply_err(req, ENOSYS);
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
    (void)ino;
    (void)size;
    (void)off;
    (void)fi;
    /* TODO: 实现 read */
    fuse_reply_err(req, ENOSYS);
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
