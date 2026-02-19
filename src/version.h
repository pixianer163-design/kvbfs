#ifndef VERSION_H
#define VERSION_H

#include "kvbfs.h"

#define KVBFS_MAX_VERSIONS 64

/* Version metadata stored per snapshot */
struct kvbfs_version_meta {
    uint64_t size;          /* file size at snapshot time */
    uint64_t blocks;        /* block count at snapshot time */
    struct timespec mtime;  /* modification time at snapshot */
};

/* KV key helpers for version storage */
static inline int kvbfs_key_version_counter(char *buf, size_t buflen, uint64_t ino)
{
    return snprintf(buf, buflen, "vc:%lu", (unsigned long)ino);
}

static inline int kvbfs_key_version_meta(char *buf, size_t buflen,
                                          uint64_t ino, uint64_t ver)
{
    return snprintf(buf, buflen, "vm:%lu:%lu",
                    (unsigned long)ino, (unsigned long)ver);
}

static inline int kvbfs_key_version_block(char *buf, size_t buflen,
                                           uint64_t ino, uint64_t ver, uint64_t block)
{
    return snprintf(buf, buflen, "vb:%lu:%lu:%lu",
                    (unsigned long)ino, (unsigned long)ver, (unsigned long)block);
}

static inline int kvbfs_key_version_meta_prefix(char *buf, size_t buflen, uint64_t ino)
{
    return snprintf(buf, buflen, "vm:%lu:", (unsigned long)ino);
}

static inline int kvbfs_key_version_block_prefix(char *buf, size_t buflen,
                                                  uint64_t ino, uint64_t ver)
{
    return snprintf(buf, buflen, "vb:%lu:%lu:",
                    (unsigned long)ino, (unsigned long)ver);
}

/* Take a snapshot of the current file content */
int version_snapshot(uint64_t ino);

/* Delete all version data for an inode */
void version_delete_all(uint64_t ino);

/* Get current version number (0 if no versions) */
uint64_t version_get_current(uint64_t ino);

/* Get version metadata; returns 0 on success */
int version_get_meta(uint64_t ino, uint64_t ver, struct kvbfs_version_meta *meta);

/* Read a block from a specific version; caller must free *data */
int version_read_block(uint64_t ino, uint64_t ver, uint64_t block,
                       char **data, size_t *len);

#endif /* VERSION_H */
