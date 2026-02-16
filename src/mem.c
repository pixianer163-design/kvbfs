#ifdef CFS_MEMORY

#include "mem.h"
#include "kvbfs.h"
#include "kv_store.h"

#include <llama.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

/* ── Worker thread ────────────────────────────────────── */

static void mem_process_task(struct mem_ctx *mem, struct mem_task *task);

static void *mem_worker(void *arg)
{
    struct mem_ctx *mem = (struct mem_ctx *)arg;

    while (1) {
        pthread_mutex_lock(&mem->lock);

        while (!mem->head && !mem->shutdown)
            pthread_cond_wait(&mem->cond, &mem->lock);

        if (mem->shutdown && !mem->head) {
            pthread_mutex_unlock(&mem->lock);
            break;
        }

        struct mem_task *task = mem->head;
        mem->head = task->next;
        if (!mem->head) mem->tail = NULL;

        pthread_mutex_unlock(&mem->lock);

        mem_process_task(mem, task);
        free(task->text);
        free(task->role);
        free(task);
    }

    return NULL;
}

/* ── Embedding ────────────────────────────────────────── */

float *mem_embed_text(struct mem_ctx *mem, const char *text, int text_len)
{
    if (!mem || !mem->ctx || !text || text_len <= 0) return NULL;

    const struct llama_vocab *vocab = llama_model_get_vocab(mem->model);

    /* Tokenize */
    int max_tokens = text_len + 16;
    llama_token *tokens = malloc(max_tokens * sizeof(llama_token));
    if (!tokens) return NULL;

    int n_tokens = llama_tokenize(vocab, text, text_len,
                                  tokens, max_tokens, true, false);
    if (n_tokens < 0) {
        /* Retry with larger buffer */
        max_tokens = -n_tokens + 16;
        tokens = realloc(tokens, max_tokens * sizeof(llama_token));
        n_tokens = llama_tokenize(vocab, text, text_len,
                                  tokens, max_tokens, true, false);
        if (n_tokens < 0) {
            free(tokens);
            return NULL;
        }
    }

    /* Truncate if exceeds context */
    if (n_tokens > mem->config.n_ctx)
        n_tokens = mem->config.n_ctx;

    /* Build batch */
    struct llama_batch batch = llama_batch_init(n_tokens, 0, 1);
    for (int i = 0; i < n_tokens; i++) {
        batch.token[i]    = tokens[i];
        batch.pos[i]      = i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i]   = false;
    }
    batch.n_tokens = n_tokens;
    free(tokens);

    /* Encode (thread-safe via embed_lock) */
    pthread_mutex_lock(&mem->embed_lock);

    llama_memory_clear(llama_get_memory(mem->ctx), true);

    if (llama_encode(mem->ctx, batch) != 0) {
        pthread_mutex_unlock(&mem->embed_lock);
        llama_batch_free(batch);
        return NULL;
    }

    /* Get pooled embedding */
    const float *embd = llama_get_embeddings_seq(mem->ctx, 0);
    if (!embd) {
        pthread_mutex_unlock(&mem->embed_lock);
        llama_batch_free(batch);
        return NULL;
    }

    int n_embd = mem->n_embd;
    float *result = malloc(n_embd * sizeof(float));
    if (!result) {
        pthread_mutex_unlock(&mem->embed_lock);
        llama_batch_free(batch);
        return NULL;
    }
    memcpy(result, embd, n_embd * sizeof(float));

    pthread_mutex_unlock(&mem->embed_lock);
    llama_batch_free(batch);

    /* L2 normalize */
    float norm = 0.0f;
    for (int i = 0; i < n_embd; i++)
        norm += result[i] * result[i];
    norm = sqrtf(norm);
    if (norm > 0.0f) {
        for (int i = 0; i < n_embd; i++)
            result[i] /= norm;
    }

    return result;
}

/* ── Sequence counter ─────────────────────────────────── */

static uint32_t mem_next_seq(void *db, uint64_t ino)
{
    char key[64];
    int keylen = snprintf(key, sizeof(key), "m:seq:%lu", (unsigned long)ino);

    char *val = NULL;
    size_t vlen = 0;
    uint32_t seq = 0;

    if (kv_get(db, key, keylen, &val, &vlen) == 0 && vlen == sizeof(uint32_t)) {
        memcpy(&seq, val, sizeof(uint32_t));
    }
    if (val) free(val);

    uint32_t next = seq + 1;
    kv_put(db, key, keylen, (const char *)&next, sizeof(uint32_t));

    return seq;
}

/* ── Generation counter ───────────────────────────────── */

uint32_t mem_next_gen(void *db, uint64_t ino)
{
    char key[64];
    int keylen = snprintf(key, sizeof(key), "m:gen:%lu", (unsigned long)ino);

    char *val = NULL;
    size_t vlen = 0;
    uint32_t gen = 0;

    if (kv_get(db, key, keylen, &val, &vlen) == 0 && vlen == sizeof(uint32_t)) {
        memcpy(&gen, val, sizeof(uint32_t));
    }
    if (val) free(val);

    uint32_t next = gen + 1;
    kv_put(db, key, keylen, (const char *)&next, sizeof(uint32_t));

    return gen;
}

/* ── Store embedding ──────────────────────────────────── */

static int mem_store_embedding(struct mem_ctx *mem, void *db, uint64_t ino,
                               uint32_t seq, const char *text, const char *role)
{
    int text_len = (int)strlen(text);
    float *vec = mem_embed_text(mem, text, text_len);
    if (!vec) {
        fprintf(stderr, "MEM: embedding failed for ino=%lu seq=%u\n",
                (unsigned long)ino, seq);
        return -1;
    }

    char key[128];
    int keylen;

    /* Store vector: m:v:<ino>:<seq> */
    keylen = snprintf(key, sizeof(key), "m:v:%lu:%u",
                      (unsigned long)ino, seq);
    kv_put(db, key, keylen, (const char *)vec,
           mem->n_embd * sizeof(float));

    /* Store text: m:t:<ino>:<seq> */
    keylen = snprintf(key, sizeof(key), "m:t:%lu:%u",
                      (unsigned long)ino, seq);
    kv_put(db, key, keylen, text, text_len);

    /* Store header: m:h:<ino>:<seq> */
    struct mem_header hdr = {0};
    hdr.ino = ino;
    hdr.seq = seq;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    hdr.timestamp = now.tv_sec;
    hdr.importance = 1.0f;
    hdr.access_count = 0;
    if (role) {
        size_t rlen = strlen(role);
        if (rlen >= sizeof(hdr.role)) rlen = sizeof(hdr.role) - 1;
        memcpy(hdr.role, role, rlen);
    }

    keylen = snprintf(key, sizeof(key), "m:h:%lu:%u",
                      (unsigned long)ino, seq);
    kv_put(db, key, keylen, (const char *)&hdr, sizeof(hdr));

    free(vec);

    printf("MEM: stored embedding ino=%lu seq=%u role=%s len=%d\n",
           (unsigned long)ino, seq, role ? role : "?", text_len);
    return 0;
}

/* ── Task processing ──────────────────────────────────── */

static void mem_process_task(struct mem_ctx *mem, struct mem_task *task)
{
    mem_store_embedding(mem, g_ctx->db, task->ino, task->seq,
                        task->text, task->role);
}

/* ── Public: memorize ─────────────────────────────────── */

int mem_memorize(struct mem_ctx *mem, void *db, uint64_t ino,
                 const char *text, const char *role)
{
    if (!mem || !mem->running || !text) return -1;

    uint32_t seq = mem_next_seq(db, ino);

    struct mem_task *task = calloc(1, sizeof(*task));
    if (!task) return -1;

    task->ino = ino;
    task->seq = seq;
    task->text = strdup(text);
    task->role = role ? strdup(role) : NULL;
    task->next = NULL;

    pthread_mutex_lock(&mem->lock);
    if (mem->tail)
        mem->tail->next = task;
    else
        mem->head = task;
    mem->tail = task;
    pthread_cond_signal(&mem->cond);
    pthread_mutex_unlock(&mem->lock);

    return 0;
}

/* ── Semantic search ──────────────────────────────────── */

static float cosine_sim(const float *a, const float *b, int n)
{
    /* Vectors are L2-normalized, so dot product = cosine similarity */
    float dot = 0.0f;
    for (int i = 0; i < n; i++)
        dot += a[i] * b[i];
    return dot;
}

int mem_search(struct mem_ctx *mem, void *db, struct cfs_mem_query *query)
{
    if (!mem || !mem->running || !query) return -1;

    /* Embed the query text */
    float *qvec = mem_embed_text(mem, query->query_text,
                                 (int)strlen(query->query_text));
    if (!qvec) return -1;

    int n_embd = mem->n_embd;
    int top_k = query->top_k;
    if (top_k <= 0) top_k = 5;
    if (top_k > CFS_MEM_MAX_RESULTS) top_k = CFS_MEM_MAX_RESULTS;

    /* Initialize results with -inf scores */
    for (int i = 0; i < top_k; i++) {
        query->results[i].score = -1.0f;
        query->results[i].summary[0] = '\0';
    }
    query->n_results = 0;

    /* Brute-force scan all m:v: keys */
    kv_iterator_t *iter = kv_iter_prefix(db, "m:v:", 4);
    while (kv_iter_valid(iter)) {
        size_t klen;
        const char *key = kv_iter_key(iter, &klen);

        /* Skip summary vectors m:v:s: */
        if (klen > 6 && strncmp(key + 4, "s:", 2) == 0) {
            kv_iter_next(iter);
            continue;
        }

        size_t vlen;
        const char *val = kv_iter_value(iter, &vlen);

        if ((int)vlen != n_embd * (int)sizeof(float)) {
            kv_iter_next(iter);
            continue;
        }

        float sim = cosine_sim(qvec, (const float *)val, n_embd);

        /* Insert into top_k (insertion sort) */
        if (sim > query->results[top_k - 1].score || query->n_results < top_k) {
            int pos = query->n_results < top_k ? query->n_results : top_k - 1;

            /* Find insertion position */
            for (int i = 0; i < pos; i++) {
                if (sim > query->results[i].score) {
                    /* Shift down */
                    for (int j = pos; j > i; j--)
                        query->results[j] = query->results[j - 1];
                    pos = i;
                    break;
                }
            }

            query->results[pos].score = sim;

            /* Parse ino and seq from key: m:v:<ino>:<seq> */
            uint64_t r_ino = 0;
            uint32_t r_seq = 0;
            sscanf(key + 4, "%lu:%u", (unsigned long *)&r_ino, &r_seq);
            query->results[pos].ino = r_ino;
            query->results[pos].seq = r_seq;

            /* Load text snippet */
            char tkey[128];
            int tkeylen = snprintf(tkey, sizeof(tkey), "m:t:%lu:%u",
                                   (unsigned long)r_ino, r_seq);
            char *tval = NULL;
            size_t tvlen = 0;
            if (kv_get(db, tkey, tkeylen, &tval, &tvlen) == 0 && tval) {
                size_t copylen = tvlen;
                if (copylen >= sizeof(query->results[pos].summary))
                    copylen = sizeof(query->results[pos].summary) - 1;
                memcpy(query->results[pos].summary, tval, copylen);
                query->results[pos].summary[copylen] = '\0';
                free(tval);
            }

            if (query->n_results < top_k)
                query->n_results++;
        }

        kv_iter_next(iter);
    }
    kv_iter_free(iter);

    free(qvec);
    return 0;
}

/* ── Init / Destroy ───────────────────────────────────── */

int mem_init(struct mem_ctx *mem, const struct mem_config *config)
{
    memset(mem, 0, sizeof(*mem));
    mem->config = *config;

    /* Load embedding model */
    struct llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = config->n_gpu_layers;

    printf("MEM: loading embedding model %s ...\n", config->embed_model_path);
    mem->model = llama_model_load_from_file(config->embed_model_path,
                                             model_params);
    if (!mem->model) {
        fprintf(stderr, "MEM: failed to load embedding model %s\n",
                config->embed_model_path);
        return -1;
    }

    mem->n_embd = llama_model_n_embd(mem->model);
    printf("MEM: embedding dim = %d\n", mem->n_embd);

    /* Create embedding context */
    struct llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = config->n_ctx;
    ctx_params.n_batch = config->n_ctx;
    ctx_params.embeddings = true;
    ctx_params.pooling_type = LLAMA_POOLING_TYPE_MEAN;

    mem->ctx = llama_init_from_model(mem->model, ctx_params);
    if (!mem->ctx) {
        fprintf(stderr, "MEM: failed to create embedding context\n");
        llama_model_free(mem->model);
        return -1;
    }

    /* Initialize locks and queue */
    pthread_mutex_init(&mem->lock, NULL);
    pthread_mutex_init(&mem->embed_lock, NULL);
    pthread_cond_init(&mem->cond, NULL);
    mem->head = NULL;
    mem->tail = NULL;
    mem->shutdown = 0;

    /* Start worker thread */
    mem->running = 1;
    if (pthread_create(&mem->thread, NULL, mem_worker, mem) != 0) {
        fprintf(stderr, "MEM: failed to create worker thread\n");
        llama_free(mem->ctx);
        llama_model_free(mem->model);
        return -1;
    }

    printf("MEM: memory subsystem initialized\n");
    return 0;
}

void mem_destroy(struct mem_ctx *mem)
{
    if (!mem || !mem->running) return;

    /* Signal thread to exit */
    pthread_mutex_lock(&mem->lock);
    mem->shutdown = 1;
    pthread_cond_signal(&mem->cond);
    pthread_mutex_unlock(&mem->lock);

    pthread_join(mem->thread, NULL);
    mem->running = 0;

    /* Drain remaining tasks */
    struct mem_task *t = mem->head;
    while (t) {
        struct mem_task *next = t->next;
        free(t->text);
        free(t->role);
        free(t);
        t = next;
    }

    /* Free llama resources */
    if (mem->ctx) llama_free(mem->ctx);
    if (mem->model) llama_model_free(mem->model);

    pthread_mutex_destroy(&mem->lock);
    pthread_mutex_destroy(&mem->embed_lock);
    pthread_cond_destroy(&mem->cond);

    printf("MEM: memory subsystem destroyed\n");
}

#endif /* CFS_MEMORY */
