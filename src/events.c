#include "events.h"
#ifdef CFS_MEMORY

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *event_type_str(enum event_type type)
{
    switch (type) {
    case EVT_CREATE:      return "create";
    case EVT_WRITE:       return "write";
    case EVT_UNLINK:      return "unlink";
    case EVT_MKDIR:       return "mkdir";
    case EVT_RMDIR:       return "rmdir";
    case EVT_RENAME:      return "rename";
    case EVT_SETATTR:     return "setattr";
    case EVT_SETXATTR:    return "setxattr";
    case EVT_REMOVEXATTR: return "removexattr";
    case EVT_LINK:        return "link";
    default:              return "unknown";
    }
}

int events_init(struct events_ctx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->ring = calloc(1, EVENTS_RING_SIZE);
    if (!ctx->ring)
        return -1;
    ctx->head = 0;
    ctx->tail = 0;
    ctx->seq = 0;
    ctx->ph = NULL;
    pthread_mutex_init(&ctx->lock, NULL);
    return 0;
}

void events_destroy(struct events_ctx *ctx)
{
    if (!ctx) return;
    pthread_mutex_lock(&ctx->lock);
    if (ctx->ph) {
        fuse_pollhandle_destroy(ctx->ph);
        ctx->ph = NULL;
    }
    free(ctx->ring);
    ctx->ring = NULL;
    pthread_mutex_unlock(&ctx->lock);
    pthread_mutex_destroy(&ctx->lock);
}

void events_emit(struct events_ctx *ctx, enum event_type type,
                 uint64_t ino, const char *path)
{
    if (!ctx || !ctx->ring) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    /* Escape path for JSON: replace " and \ */
    char escaped_path[512];
    size_t ei = 0;
    if (path) {
        for (size_t i = 0; path[i] && ei < sizeof(escaped_path) - 2; i++) {
            if (path[i] == '"' || path[i] == '\\') {
                escaped_path[ei++] = '\\';
            }
            escaped_path[ei++] = path[i];
        }
    }
    escaped_path[ei] = '\0';

    char line[1024];
    int len;

    pthread_mutex_lock(&ctx->lock);

    ctx->seq++;
    len = snprintf(line, sizeof(line),
                   "{\"seq\":%lu,\"type\":\"%s\",\"ino\":%lu,\"path\":\"%s\",\"ts\":%lu}\n",
                   (unsigned long)ctx->seq,
                   event_type_str(type),
                   (unsigned long)ino,
                   escaped_path,
                   (unsigned long)ts.tv_sec);

    if (len <= 0 || (size_t)len >= sizeof(line)) {
        pthread_mutex_unlock(&ctx->lock);
        return;
    }

    /* Write into ring buffer */
    for (int i = 0; i < len; i++) {
        ctx->ring[ctx->head % EVENTS_RING_SIZE] = line[i];
        ctx->head++;
    }

    /* If we've overwritten the tail, advance it to the next newline */
    if (ctx->head - ctx->tail > EVENTS_RING_SIZE) {
        ctx->tail = ctx->head - EVENTS_RING_SIZE;
        /* Advance tail to next newline to maintain line integrity */
        while (ctx->tail < ctx->head &&
               ctx->ring[ctx->tail % EVENTS_RING_SIZE] != '\n') {
            ctx->tail++;
        }
        if (ctx->tail < ctx->head)
            ctx->tail++; /* skip past the newline */
    }

    /* Notify poll waiters */
    if (ctx->ph) {
        fuse_lowlevel_notify_poll(ctx->ph);
        fuse_pollhandle_destroy(ctx->ph);
        ctx->ph = NULL;
    }

    pthread_mutex_unlock(&ctx->lock);
}

#endif /* CFS_MEMORY */
