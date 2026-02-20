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
    if (!le) { pthread_mutex_unlock(&vt->lock); return vino; }
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
