#include "kvbfs.h"
#include "kv_store.h"
#include "inode.h"

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
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
    (void)fi;

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

    fuse_reply_attr(req, &st, 1.0);
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
    inode_delete(child_ino);

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

    /* 返回新文件信息 */
    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.ino = ic->inode.ino;
    e.attr_timeout = 1.0;
    e.entry_timeout = 1.0;

    pthread_rwlock_rdlock(&ic->lock);
    inode_to_stat(&ic->inode, &e.attr);
    pthread_rwlock_unlock(&ic->lock);

    inode_put(ic);

    fuse_reply_create(req, &e, fi);
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
        inode_delete(child_ino);
    }

    fuse_reply_err(req, 0);
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

    fuse_reply_open(req, fi);
}

static void kvbfs_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    (void)ino;
    (void)fi;
    /* 目前不需要特殊处理 */
    fuse_reply_err(req, 0);
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
    (void)fi;

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
};
