#ifndef MEM_H
#define MEM_H
#ifdef CFS_MEMORY

#include <pthread.h>
#include <stdint.h>

struct mem_config {
    const char *embed_model_path;
    int n_ctx;
    int n_gpu_layers;
};

struct mem_task {
    uint64_t ino;
    uint32_t seq;
    char *text;
    char *role;
    struct mem_task *next;
};

struct mem_header {
    uint64_t ino;
    uint32_t seq;
    int64_t  timestamp;
    float    importance;
    uint32_t access_count;
    char     role[16];
};

struct mem_edge {
    int64_t  timestamp;
    float    confidence;
    uint64_t source_ino;
    uint32_t source_gen;
};

struct mem_ctx {
    struct llama_model *model;
    struct llama_context *ctx;
    int n_embd;

    pthread_t thread;
    pthread_mutex_t lock;
    pthread_mutex_t embed_lock;   /* protects ctx concurrent access */
    pthread_cond_t cond;
    struct mem_task *head, *tail;
    int shutdown;
    int running;

    struct mem_config config;
};

int   mem_init(struct mem_ctx *mem, const struct mem_config *config);
void  mem_destroy(struct mem_ctx *mem);
float *mem_embed_text(struct mem_ctx *mem, const char *text, int text_len);
int   mem_memorize(struct mem_ctx *mem, void *db, uint64_t ino,
                   const char *text, const char *role);
int   mem_index_file(struct mem_ctx *mem, void *db, uint64_t ino);
void  mem_delete_embeddings(void *db, uint64_t ino);
uint32_t mem_next_gen(void *db, uint64_t ino);
struct cfs_mem_query;
int   mem_search(struct mem_ctx *mem, void *db, struct cfs_mem_query *query);

#endif /* CFS_MEMORY */
#endif /* MEM_H */
