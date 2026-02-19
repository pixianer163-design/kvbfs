#ifndef EVENTS_H
#define EVENTS_H
#ifdef CFS_MEMORY

#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

#define FUSE_USE_VERSION 35
#include <fuse_lowlevel.h>

#define AGENTFS_EVENTS_INO  0xFFFFFFFFFFFFFEULL
#define AGENTFS_EVENTS_NAME ".events"
#define EVENTS_RING_SIZE    (256 * 1024)  /* 256 KB ring buffer */

enum event_type {
    EVT_CREATE,
    EVT_WRITE,
    EVT_UNLINK,
    EVT_MKDIR,
    EVT_RMDIR,
    EVT_RENAME,
    EVT_SETATTR,
    EVT_SETXATTR,
    EVT_REMOVEXATTR,
    EVT_LINK
};

struct events_ctx {
    char           *ring;          /* circular buffer */
    size_t          head;          /* write position (always advances) */
    size_t          tail;          /* oldest data position */
    uint64_t        seq;           /* monotonic sequence number */
    pthread_mutex_t lock;
    struct fuse_pollhandle *ph;    /* for async notification */
};

/* Per-open state for .events file */
struct events_fh {
    uint64_t start_seq;   /* sequence at open time */
    size_t   read_pos;    /* current read position in ring */
};

int  events_init(struct events_ctx *ctx);
void events_destroy(struct events_ctx *ctx);
void events_emit(struct events_ctx *ctx, enum event_type type,
                 uint64_t ino, const char *path);

#endif /* CFS_MEMORY */
#endif /* EVENTS_H */
