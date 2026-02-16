#ifdef CFS_LOCAL_LLM

#include "kvbfs.h"
#include "llm.h"
#include "kv_store.h"
#include "inode.h"
#ifdef CFS_MEMORY
#include "mem.h"
#endif

#include <llama.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

/* ── 文件内容读写辅助 ──────────────────────────────────── */

/* 读取 inode 对应文件的全部内容，返回 malloc 的缓冲区，len 输出实际长度 */
static char *file_read_all(uint64_t ino, size_t *len)
{
    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) return NULL;

    pthread_rwlock_rdlock(&ic->lock);
    uint64_t file_size = ic->inode.size;
    uint64_t blocks = ic->inode.blocks;
    pthread_rwlock_unlock(&ic->lock);
    inode_put(ic);

    if (file_size == 0) {
        *len = 0;
        return NULL;
    }

    char *buf = malloc(file_size + 1);
    if (!buf) return NULL;

    size_t total = 0;
    for (uint64_t i = 0; i < blocks && total < file_size; i++) {
        char key[64];
        int keylen = kvbfs_key_block(key, sizeof(key), ino, i);

        char *block = NULL;
        size_t blen = 0;
        if (kv_get(g_ctx->db, key, keylen, &block, &blen) == 0) {
            size_t copy = blen;
            if (total + copy > file_size) copy = file_size - total;
            memcpy(buf + total, block, copy);
            free(block);
            total += copy;
        } else {
            /* 空洞：零填充 */
            size_t fill = KVBFS_BLOCK_SIZE;
            if (total + fill > file_size) fill = file_size - total;
            memset(buf + total, 0, fill);
            total += fill;
        }
    }

    buf[total] = '\0';
    *len = total;
    return buf;
}

/* 追加数据到 inode 文件末尾 */
static int file_append(uint64_t ino, const char *data, size_t data_len)
{
    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) return -1;

    pthread_rwlock_rdlock(&ic->lock);
    uint64_t off = ic->inode.size;
    pthread_rwlock_unlock(&ic->lock);

    /* 逐块写入 */
    size_t written = 0;
    uint64_t block_idx = off / KVBFS_BLOCK_SIZE;
    size_t block_off = off % KVBFS_BLOCK_SIZE;

    while (written < data_len) {
        char key[64];
        int keylen = kvbfs_key_block(key, sizeof(key), ino, block_idx);

        char block[KVBFS_BLOCK_SIZE];
        memset(block, 0, KVBFS_BLOCK_SIZE);

        /* 读取现有块 */
        char *existing = NULL;
        size_t elen = 0;
        if (kv_get(g_ctx->db, key, keylen, &existing, &elen) == 0) {
            size_t clen = elen < KVBFS_BLOCK_SIZE ? elen : KVBFS_BLOCK_SIZE;
            memcpy(block, existing, clen);
            free(existing);
        }

        size_t to_write = KVBFS_BLOCK_SIZE - block_off;
        if (to_write > data_len - written) to_write = data_len - written;
        memcpy(block + block_off, data + written, to_write);

        if (kv_put(g_ctx->db, key, keylen, block, KVBFS_BLOCK_SIZE) != 0) {
            inode_put(ic);
            return -1;
        }

        written += to_write;
        block_idx++;
        block_off = 0;
    }

    /* 更新 inode */
    pthread_rwlock_wrlock(&ic->lock);
    ic->inode.size = off + data_len;
    ic->inode.blocks = (ic->inode.size + KVBFS_BLOCK_SIZE - 1) / KVBFS_BLOCK_SIZE;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    ic->inode.mtime = now;
    ic->inode.ctime = now;
    pthread_rwlock_unlock(&ic->lock);

    inode_sync(ic);
    inode_put(ic);
    return 0;
}

/* ── 对话协议辅助 ──────────────────────────────────────── */

/* 检查文件内容末尾是否有未回复的 User: 消息 */
static bool needs_response(const char *content, size_t len)
{
    if (!content || len == 0) return false;

    /* 从末尾向前找最后一个非空行的行首 */
    const char *end = content + len;

    /* 跳过末尾换行 */
    while (end > content && (*(end - 1) == '\n' || *(end - 1) == '\r'))
        end--;

    if (end == content) return false;

    /* 找这一行的行首 */
    const char *line = end;
    while (line > content && *(line - 1) != '\n')
        line--;

    /* 检查是否以 "User:" 开头 */
    size_t line_len = end - line;
    if (line_len >= 5 && strncmp(line, "User:", 5) == 0)
        return true;

    return false;
}

/* 构建聊天消息数组给 llama.cpp */
struct chat_msg {
    const char *role;
    const char *content;
};

static int parse_conversation(const char *text, struct chat_msg **msgs, int *count)
{
    *msgs = NULL;
    *count = 0;

    int capacity = 16;
    struct chat_msg *arr = malloc(capacity * sizeof(struct chat_msg));
    if (!arr) return -1;

    const char *p = text;
    while (*p) {
        /* 跳过空行 */
        while (*p == '\n' || *p == '\r') p++;
        if (!*p) break;

        const char *role = NULL;
        const char *start = NULL;

        if (strncmp(p, "User:", 5) == 0) {
            role = "user";
            start = p + 5;
        } else if (strncmp(p, "Assistant:", 10) == 0) {
            role = "assistant";
            start = p + 10;
        } else {
            /* 跳过无法解析的行 */
            while (*p && *p != '\n') p++;
            continue;
        }

        /* 跳过 role 后的空格 */
        while (*start == ' ') start++;

        /* 找到这一轮消息的结尾（下一个 User:/Assistant: 或 EOF） */
        const char *end = start;
        while (*end) {
            if (*end == '\n') {
                const char *next = end + 1;
                if (strncmp(next, "User:", 5) == 0 ||
                    strncmp(next, "Assistant:", 10) == 0)
                    break;
            }
            end++;
        }

        /* 去除尾部空白 */
        const char *trim = end;
        while (trim > start && (*(trim - 1) == '\n' || *(trim - 1) == '\r' || *(trim - 1) == ' '))
            trim--;

        if (*count >= capacity) {
            capacity *= 2;
            arr = realloc(arr, capacity * sizeof(struct chat_msg));
            if (!arr) return -1;
        }

        size_t clen = trim - start;
        char *cbuf = malloc(clen + 1);
        memcpy(cbuf, start, clen);
        cbuf[clen] = '\0';

        arr[*count].role = role;
        arr[*count].content = cbuf;
        (*count)++;

        p = end;
    }

    *msgs = arr;
    return 0;
}

static void free_conversation(struct chat_msg *msgs, int count)
{
    for (int i = 0; i < count; i++)
        free((void *)msgs[i].content);
    free(msgs);
}

/* ── 生成状态管理 ──────────────────────────────────────── */

static struct llm_gen_state *gen_state_find(struct llm_ctx *llm, uint64_t ino)
{
    struct llm_gen_state *gs = NULL;
    HASH_FIND(hh, llm->gen_states, &ino, sizeof(ino), gs);
    return gs;
}

void llm_gen_start(struct llm_ctx *llm, uint64_t ino)
{
    pthread_mutex_lock(&llm->gen_lock);

    struct llm_gen_state *gs = gen_state_find(llm, ino);
    if (!gs) {
        gs = calloc(1, sizeof(*gs));
        gs->ino = ino;
        HASH_ADD(hh, llm->gen_states, ino, sizeof(ino), gs);
    }
    gs->generating = 1;

    pthread_mutex_unlock(&llm->gen_lock);
}

void llm_gen_finish(struct llm_ctx *llm, uint64_t ino)
{
    pthread_mutex_lock(&llm->gen_lock);

    struct llm_gen_state *gs = gen_state_find(llm, ino);
    if (gs) {
        gs->generating = 0;

        /* 通知所有 poll waiter */
        for (int i = 0; i < gs->n_waiters; i++) {
            fuse_lowlevel_notify_poll(gs->waiters[i]);
            fuse_pollhandle_destroy(gs->waiters[i]);
        }
        gs->n_waiters = 0;
    }

    pthread_mutex_unlock(&llm->gen_lock);
}

int llm_gen_is_active(struct llm_ctx *llm, uint64_t ino)
{
    pthread_mutex_lock(&llm->gen_lock);

    struct llm_gen_state *gs = gen_state_find(llm, ino);
    int active = (gs && gs->generating);

    pthread_mutex_unlock(&llm->gen_lock);
    return active;
}

void llm_gen_add_waiter(struct llm_ctx *llm, uint64_t ino, struct fuse_pollhandle *ph)
{
    pthread_mutex_lock(&llm->gen_lock);

    struct llm_gen_state *gs = gen_state_find(llm, ino);
    if (gs && gs->n_waiters < LLM_MAX_POLL_WAITERS) {
        gs->waiters[gs->n_waiters++] = ph;
    } else {
        /* 无状态或满了，立即释放 */
        fuse_pollhandle_destroy(ph);
    }

    pthread_mutex_unlock(&llm->gen_lock);
}

void llm_gen_destroy(struct llm_ctx *llm)
{
    struct llm_gen_state *gs, *tmp;
    HASH_ITER(hh, llm->gen_states, gs, tmp) {
        /* 释放残留 waiter */
        for (int i = 0; i < gs->n_waiters; i++)
            fuse_pollhandle_destroy(gs->waiters[i]);
        HASH_DEL(llm->gen_states, gs);
        free(gs);
    }
}

/* ── 文件覆写辅助 ─────────────────────────────────────── */

/* Overwrite inode file contents, deleting old blocks first */
static int file_overwrite(uint64_t ino, const char *data, size_t data_len)
{
    struct kvbfs_inode_cache *ic = inode_get(ino);
    if (!ic) return -1;

    /* Delete all existing blocks */
    pthread_rwlock_rdlock(&ic->lock);
    uint64_t old_blocks = ic->inode.blocks;
    pthread_rwlock_unlock(&ic->lock);

    for (uint64_t i = 0; i < old_blocks; i++) {
        char key[64];
        int keylen = kvbfs_key_block(key, sizeof(key), ino, i);
        kv_delete(g_ctx->db, key, keylen);
    }

    /* Reset inode size */
    pthread_rwlock_wrlock(&ic->lock);
    ic->inode.size = 0;
    ic->inode.blocks = 0;
    pthread_rwlock_unlock(&ic->lock);
    inode_sync(ic);
    inode_put(ic);

    /* Write new data using file_append */
    if (data_len > 0)
        return file_append(ino, data, data_len);
    return 0;
}

#ifdef CFS_MEMORY
/* ── LLM summarization ───────────────────────────────── */

/* Use the chat LLM to generate a summary of conversation text.
 * Returns malloc'd summary string, caller frees. */
static char *llm_summarize(struct llm_ctx *llm, const char *text,
                           int text_len, size_t *out_len)
{
    /* Build a 2-message conversation: system + user */
    struct llama_chat_message chat[2];
    chat[0].role = "system";
    chat[0].content = "You are a summarizer. Condense the following conversation "
                      "into a brief summary preserving key facts, decisions, and "
                      "context. Output only the summary, no preamble.";
    chat[1].role = "user";

    /* Build user content: "Summarize:\n<text>" */
    const char *prefix = "Summarize the following conversation:\n";
    size_t plen = strlen(prefix);
    char *user_content = malloc(plen + text_len + 1);
    if (!user_content) return NULL;
    memcpy(user_content, prefix, plen);
    memcpy(user_content + plen, text, text_len);
    user_content[plen + text_len] = '\0';
    chat[1].content = user_content;

    /* Apply chat template */
    const char *tmpl = llama_model_chat_template(llm->model, NULL);
    int prompt_len = llama_chat_apply_template(tmpl, chat, 2, true, NULL, 0);
    if (prompt_len < 0) {
        free(user_content);
        return NULL;
    }

    char *prompt = malloc(prompt_len + 1);
    llama_chat_apply_template(tmpl, chat, 2, true, prompt, prompt_len + 1);
    prompt[prompt_len] = '\0';
    free(user_content);

    /* Tokenize */
    const struct llama_vocab *vocab = llama_model_get_vocab(llm->model);
    int n_tokens_max = llm->config.n_ctx;
    llama_token *tokens = malloc(n_tokens_max * sizeof(llama_token));
    int n_tokens = llama_tokenize(vocab, prompt, prompt_len,
                                  tokens, n_tokens_max, true, true);
    free(prompt);

    if (n_tokens < 0) {
        free(tokens);
        return NULL;
    }

    /* Decode prompt */
    llama_memory_clear(llama_get_memory(llm->ctx), true);

    int batch_size = 512;
    for (int i = 0; i < n_tokens; i += batch_size) {
        int n = n_tokens - i;
        if (n > batch_size) n = batch_size;
        struct llama_batch batch = llama_batch_get_one(tokens + i, n);
        if (llama_decode(llm->ctx, batch) != 0) {
            free(tokens);
            return NULL;
        }
    }
    free(tokens);

    /* Generate summary (max 256 tokens, low temperature) */
    char *summary = malloc(256 * 16);
    size_t sum_len = 0;

    struct llama_sampler *sampler = llama_sampler_chain_init(
        llama_sampler_chain_default_params());
    llama_sampler_chain_add(sampler, llama_sampler_init_temp(0.3f));
    llama_sampler_chain_add(sampler, llama_sampler_init_dist(0));

    for (int i = 0; i < 256; i++) {
        llama_token token = llama_sampler_sample(sampler, llm->ctx, -1);
        if (llama_vocab_is_eog(vocab, token))
            break;

        char piece[256];
        int piece_len = llama_token_to_piece(
            vocab, token, piece, sizeof(piece), 0, true);
        if (piece_len > 0) {
            memcpy(summary + sum_len, piece, piece_len);
            sum_len += piece_len;
        }

        struct llama_batch batch = llama_batch_get_one(&token, 1);
        if (llama_decode(llm->ctx, batch) != 0)
            break;
    }

    llama_sampler_free(sampler);

    summary[sum_len] = '\0';
    if (out_len) *out_len = sum_len;

    printf("MEM: generated summary (%zu bytes)\n", sum_len);
    return summary;
}

/* ── Context compression ──────────────────────────────── */

/* Compress conversation context when it exceeds threshold.
 * Returns 1 if compression was performed, 0 otherwise, -1 on error. */
static int try_compress_context(struct llm_ctx *llm, uint64_t ino,
                                const char *content, size_t len)
{
    /* Tokenize to check length */
    const struct llama_vocab *vocab = llama_model_get_vocab(llm->model);
    int n_ctx = llm->config.n_ctx;
    int n_tokens_max = n_ctx;
    llama_token *tokens = malloc(n_tokens_max * sizeof(llama_token));
    int n_tokens = llama_tokenize(vocab, content, (int)len,
                                  tokens, n_tokens_max, true, true);
    free(tokens);

    /* If tokenization failed (too long), also compress */
    int threshold = (int)(n_ctx * 0.75);
    if (n_tokens >= 0 && n_tokens < threshold)
        return 0;

    printf("MEM: context overflow detected (tokens=%d, threshold=%d), compressing...\n",
           n_tokens, threshold);

    /* Parse conversation */
    struct chat_msg *msgs = NULL;
    int msg_count = 0;
    if (parse_conversation(content, &msgs, &msg_count) != 0 || msg_count < 2) {
        free_conversation(msgs, msg_count);
        return -1;
    }

    /* Split: 60% old, 40% recent */
    int split = (int)(msg_count * 0.6);
    if (split < 1) split = 1;
    if (split >= msg_count) split = msg_count - 1;

    /* Build old text for summarization */
    size_t old_text_size = 0;
    for (int i = 0; i < split; i++)
        old_text_size += strlen(msgs[i].content) + 20; /* role prefix + newline */

    char *old_text = malloc(old_text_size + 1);
    size_t old_off = 0;
    for (int i = 0; i < split; i++) {
        const char *role_prefix = strcmp(msgs[i].role, "user") == 0
                                  ? "User: " : "Assistant: ";
        int n = snprintf(old_text + old_off, old_text_size - old_off + 1,
                         "%s%s\n", role_prefix, msgs[i].content);
        if (n > 0) old_off += n;
    }

    /* Archive old messages */
    uint32_t gen = mem_next_gen(g_ctx->db, ino);
    for (int i = 0; i < split; i++) {
        char key[128];
        int keylen = snprintf(key, sizeof(key), "m:a:%lu:%u:%d",
                              (unsigned long)ino, gen, i);
        const char *role_prefix = strcmp(msgs[i].role, "user") == 0
                                  ? "User: " : "Assistant: ";
        size_t entry_len = strlen(role_prefix) + strlen(msgs[i].content) + 1;
        char *entry = malloc(entry_len + 1);
        snprintf(entry, entry_len + 1, "%s%s\n", role_prefix, msgs[i].content);
        kv_put(g_ctx->db, key, keylen, entry, strlen(entry));
        free(entry);
    }

    /* Generate summary */
    size_t sum_len = 0;
    char *summary = llm_summarize(llm, old_text, (int)old_off, &sum_len);
    free(old_text);

    if (!summary || sum_len == 0) {
        free(summary);
        free_conversation(msgs, msg_count);
        return -1;
    }

    /* Store summary text: m:s:<ino>:<gen> */
    {
        char key[128];
        int keylen = snprintf(key, sizeof(key), "m:s:%lu:%u",
                              (unsigned long)ino, gen);
        kv_put(g_ctx->db, key, keylen, summary, sum_len);
    }

    /* Embed summary: m:v:s:<ino>:<gen> */
    if (g_ctx->mem.running) {
        float *svec = mem_embed_text(&g_ctx->mem, summary, (int)sum_len);
        if (svec) {
            char key[128];
            int keylen = snprintf(key, sizeof(key), "m:v:s:%lu:%u",
                                  (unsigned long)ino, gen);
            kv_put(g_ctx->db, key, keylen, (const char *)svec,
                   g_ctx->mem.n_embd * sizeof(float));
            free(svec);
        }
    }

    /* Rewrite session file: [Context Summary]\n<summary>\n---\n<recent msgs> */
    size_t new_size = 32 + sum_len + 8; /* header + summary + separator */
    for (int i = split; i < msg_count; i++)
        new_size += strlen(msgs[i].content) + 20;

    char *new_content = malloc(new_size + 1);
    size_t off = 0;
    off += snprintf(new_content + off, new_size - off + 1,
                    "[Context Summary]\n%s\n---\n", summary);

    for (int i = split; i < msg_count; i++) {
        const char *role_prefix = strcmp(msgs[i].role, "user") == 0
                                  ? "User: " : "Assistant: ";
        off += snprintf(new_content + off, new_size - off + 1,
                        "%s%s\n", role_prefix, msgs[i].content);
    }

    free(summary);
    free_conversation(msgs, msg_count);

    /* Overwrite the session file */
    file_overwrite(ino, new_content, off);
    free(new_content);

    printf("MEM: compressed context for ino=%lu gen=%u\n",
           (unsigned long)ino, gen);
    return 1;
}
#endif /* CFS_MEMORY */

/* ── 推理核心 ─────────────────────────────────────────── */

static void process_session(struct llm_ctx *llm, uint64_t ino)
{
    /* 1. 读取文件内容 */
    size_t len = 0;
    char *content = file_read_all(ino, &len);
    if (!content || !needs_response(content, len)) {
        free(content);
        return;
    }

    /* 2. 标记开始生成 */
    llm_gen_start(llm, ino);

#ifdef CFS_MEMORY
    /* Check for context overflow and compress if needed */
    if (g_ctx->mem.running) {
        int compressed = try_compress_context(llm, ino, content, len);
        if (compressed > 0) {
            /* Re-read compressed file */
            free(content);
            content = file_read_all(ino, &len);
            if (!content || !needs_response(content, len)) {
                free(content);
                llm_gen_finish(llm, ino);
                return;
            }
        }
    }
#endif

    /* 3. 解析对话历史 */
    struct chat_msg *msgs = NULL;
    int msg_count = 0;
    if (parse_conversation(content, &msgs, &msg_count) != 0 || msg_count == 0) {
        free(content);
        llm_gen_finish(llm, ino);
        return;
    }
    free(content);

    /* 4. 构建 llama.cpp chat 消息 */
    struct llama_chat_message *chat = calloc(msg_count, sizeof(struct llama_chat_message));
    for (int i = 0; i < msg_count; i++) {
        chat[i].role = msgs[i].role;
        chat[i].content = msgs[i].content;
    }

    /* 应用 chat template */
    const struct llama_model *model = llm->model;
    const char *tmpl = llama_model_chat_template(model, NULL);

    /* 先计算需要的缓冲区大小 */
    int prompt_len = llama_chat_apply_template(
        tmpl, chat, msg_count, true, NULL, 0);

    if (prompt_len < 0) {
        fprintf(stderr, "LLM: failed to apply chat template\n");
        free(chat);
        free_conversation(msgs, msg_count);
        llm_gen_finish(llm, ino);
        return;
    }

    char *prompt = malloc(prompt_len + 1);
    llama_chat_apply_template(tmpl, chat, msg_count, true, prompt, prompt_len + 1);
    prompt[prompt_len] = '\0';

#ifdef CFS_MEMORY
    /* Save last user message before freeing conversation */
    char *last_user_msg = NULL;
    if (g_ctx->mem.running) {
        for (int i = msg_count - 1; i >= 0; i--) {
            if (strcmp(msgs[i].role, "user") == 0) {
                last_user_msg = strdup(msgs[i].content);
                break;
            }
        }
    }
#endif

    free(chat);
    free_conversation(msgs, msg_count);

    /* 5. Tokenize */
    const struct llama_vocab *vocab = llama_model_get_vocab(llm->model);
    int n_tokens_max = llm->config.n_ctx;
    llama_token *tokens = malloc(n_tokens_max * sizeof(llama_token));
    int n_tokens = llama_tokenize(
        vocab, prompt, prompt_len,
        tokens, n_tokens_max, true, true);
    free(prompt);

    if (n_tokens < 0) {
        fprintf(stderr, "LLM: tokenization failed\n");
        free(tokens);
#ifdef CFS_MEMORY
        free(last_user_msg);
#endif
        llm_gen_finish(llm, ino);
        return;
    }

    /* 6. 清空 KV cache 并 decode prompt */
    llama_memory_clear(llama_get_memory(llm->ctx), true);

    /* 逐批 decode prompt tokens */
    int batch_size = 512;
    for (int i = 0; i < n_tokens; i += batch_size) {
        int n = n_tokens - i;
        if (n > batch_size) n = batch_size;

        struct llama_batch batch = llama_batch_get_one(tokens + i, n);
        if (llama_decode(llm->ctx, batch) != 0) {
            fprintf(stderr, "LLM: prompt decode failed\n");
            free(tokens);
#ifdef CFS_MEMORY
            free(last_user_msg);
#endif
            llm_gen_finish(llm, ino);
            return;
        }
    }

    /* 7. 自回归生成 */
    char *response = malloc(llm->config.max_tokens * 16); /* 足够大 */
    size_t resp_len = 0;
    int max_tokens = llm->config.max_tokens;

    struct llama_sampler *sampler = llama_sampler_chain_init(
        llama_sampler_chain_default_params());
    llama_sampler_chain_add(sampler, llama_sampler_init_temp(llm->config.temperature));
    llama_sampler_chain_add(sampler, llama_sampler_init_dist(0));

    for (int i = 0; i < max_tokens; i++) {
        llama_token token = llama_sampler_sample(sampler, llm->ctx, -1);

        /* 检查 EOS */
        if (llama_vocab_is_eog(vocab, token))
            break;

        /* token → text */
        char piece[256];
        int piece_len = llama_token_to_piece(
            vocab, token, piece, sizeof(piece), 0, true);
        if (piece_len > 0) {
            memcpy(response + resp_len, piece, piece_len);
            resp_len += piece_len;
        }

        /* decode 这个 token 以准备下一次采样 */
        struct llama_batch batch = llama_batch_get_one(&token, 1);
        if (llama_decode(llm->ctx, batch) != 0)
            break;
    }

    llama_sampler_free(sampler);
    free(tokens);

    /* 8. 追加 "Assistant: <response>\n" 到文件 */
    if (resp_len > 0) {
        const char *prefix = "Assistant: ";
        size_t prefix_len = strlen(prefix);
        size_t total = prefix_len + resp_len + 1; /* +1 for '\n' */
        char *output = malloc(total);
        memcpy(output, prefix, prefix_len);
        memcpy(output + prefix_len, response, resp_len);
        output[total - 1] = '\n';

        file_append(ino, output, total);
        free(output);
    }

#ifdef CFS_MEMORY
    /* Memorize user and assistant messages */
    if (g_ctx->mem.running && last_user_msg) {
        mem_memorize(&g_ctx->mem, g_ctx->db, ino, last_user_msg, "user");
        if (resp_len > 0) {
            /* null-terminate response for memorization */
            response[resp_len] = '\0';
            mem_memorize(&g_ctx->mem, g_ctx->db, ino, response, "assistant");
        }
    }
    free(last_user_msg);
#endif

    free(response);

    /* 9. 标记生成完成，通知 poll waiter */
    llm_gen_finish(llm, ino);

    printf("LLM: generated response for inode %lu (%zu bytes)\n",
           (unsigned long)ino, resp_len);
}

/* ── 推理线程 ─────────────────────────────────────────── */

static void *llm_worker(void *arg)
{
    struct llm_ctx *llm = (struct llm_ctx *)arg;

    while (1) {
        pthread_mutex_lock(&llm->lock);

        while (!llm->head && !llm->shutdown)
            pthread_cond_wait(&llm->cond, &llm->lock);

        if (llm->shutdown && !llm->head) {
            pthread_mutex_unlock(&llm->lock);
            break;
        }

        /* 取出队列头部任务 */
        struct llm_task *task = llm->head;
        llm->head = task->next;
        if (!llm->head) llm->tail = NULL;

        pthread_mutex_unlock(&llm->lock);

        /* 处理任务 */
        process_session(llm, task->ino);
        free(task);
    }

    return NULL;
}

/* ── 公共接口 ─────────────────────────────────────────── */

int llm_init(struct llm_ctx *llm, const struct llm_config *config)
{
    memset(llm, 0, sizeof(*llm));
    llm->config = *config;

    /* 初始化 llama.cpp 后端 */
    llama_backend_init();

    /* 加载模型 */
    struct llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = config->n_gpu_layers;

    printf("LLM: loading model %s ...\n", config->model_path);
    llm->model = llama_model_load_from_file(config->model_path, model_params);
    if (!llm->model) {
        fprintf(stderr, "LLM: failed to load model %s\n", config->model_path);
        return -1;
    }
    printf("LLM: model loaded\n");

    /* 创建推理上下文 */
    struct llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = config->n_ctx;
    ctx_params.n_batch = 512;

    llm->ctx = llama_init_from_model(llm->model, ctx_params);
    if (!llm->ctx) {
        fprintf(stderr, "LLM: failed to create context\n");
        llama_model_free(llm->model);
        return -1;
    }

    /* 初始化任务队列 */
    pthread_mutex_init(&llm->lock, NULL);
    pthread_cond_init(&llm->cond, NULL);
    llm->head = NULL;
    llm->tail = NULL;
    llm->shutdown = 0;

    /* 初始化生成状态 */
    llm->gen_states = NULL;
    pthread_mutex_init(&llm->gen_lock, NULL);

    /* 启动推理线程 */
    llm->running = 1;
    if (pthread_create(&llm->thread, NULL, llm_worker, llm) != 0) {
        fprintf(stderr, "LLM: failed to create worker thread\n");
        llama_free(llm->ctx);
        llama_model_free(llm->model);
        return -1;
    }

    printf("LLM: inference thread started\n");
    return 0;
}

void llm_destroy(struct llm_ctx *llm)
{
    if (!llm->running) return;

    /* 通知线程退出 */
    pthread_mutex_lock(&llm->lock);
    llm->shutdown = 1;
    pthread_cond_signal(&llm->cond);
    pthread_mutex_unlock(&llm->lock);

    pthread_join(llm->thread, NULL);
    llm->running = 0;

    /* 清空剩余任务 */
    struct llm_task *t = llm->head;
    while (t) {
        struct llm_task *next = t->next;
        free(t);
        t = next;
    }

    /* 释放 llama 资源 */
    if (llm->ctx) llama_free(llm->ctx);
    if (llm->model) llama_model_free(llm->model);

    llama_backend_free();

    pthread_mutex_destroy(&llm->lock);
    pthread_cond_destroy(&llm->cond);

    /* 清理生成状态 */
    llm_gen_destroy(llm);
    pthread_mutex_destroy(&llm->gen_lock);

    printf("LLM: subsystem destroyed\n");
}

int llm_submit(struct llm_ctx *llm, uint64_t ino)
{
    struct llm_task *task = malloc(sizeof(struct llm_task));
    if (!task) return -1;

    task->ino = ino;
    task->next = NULL;

    pthread_mutex_lock(&llm->lock);

    /* 去重：如果同一 ino 已在队列中，跳过 */
    struct llm_task *p = llm->head;
    while (p) {
        if (p->ino == ino) {
            pthread_mutex_unlock(&llm->lock);
            free(task);
            return 0;
        }
        p = p->next;
    }

    if (llm->tail)
        llm->tail->next = task;
    else
        llm->head = task;
    llm->tail = task;

    pthread_cond_signal(&llm->cond);
    pthread_mutex_unlock(&llm->lock);

    return 0;
}

#endif /* CFS_LOCAL_LLM */
