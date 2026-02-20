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

/* Reverse lookup entry: (parent_vino, child_name) -> vino */
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
