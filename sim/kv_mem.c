#include "kv_mem.h"
#include "uthash.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* 哈希表条目 */
struct kv_entry {
    char   *key;
    size_t  key_len;
    char   *value;
    size_t  value_len;
    UT_hash_handle hh;
};

struct kv_mem {
    struct kv_entry *table;       /* uthash 头指针 */
    pthread_mutex_t  lock;
};

kv_mem_t *kv_mem_create(void)
{
    kv_mem_t *mem = calloc(1, sizeof(*mem));
    if (!mem)
        return NULL;
    pthread_mutex_init(&mem->lock, NULL);
    return mem;
}

void kv_mem_destroy(kv_mem_t *mem)
{
    if (!mem)
        return;

    struct kv_entry *cur, *tmp;
    HASH_ITER(hh, mem->table, cur, tmp) {
        HASH_DEL(mem->table, cur);
        free(cur->key);
        free(cur->value);
        free(cur);
    }
    pthread_mutex_destroy(&mem->lock);
    free(mem);
}

int kv_mem_store(kv_mem_t *mem, const char *key, size_t key_len,
                 const char *value, size_t value_len)
{
    pthread_mutex_lock(&mem->lock);

    /* 查找是否已存在 */
    struct kv_entry *entry = NULL;
    HASH_FIND(hh, mem->table, key, key_len, entry);

    if (entry) {
        /* 更新现有条目 */
        char *new_val = malloc(value_len);
        if (!new_val) {
            pthread_mutex_unlock(&mem->lock);
            return -1;
        }
        memcpy(new_val, value, value_len);
        free(entry->value);
        entry->value = new_val;
        entry->value_len = value_len;
    } else {
        /* 新建条目 */
        entry = malloc(sizeof(*entry));
        if (!entry) {
            pthread_mutex_unlock(&mem->lock);
            return -1;
        }
        entry->key = malloc(key_len);
        entry->value = malloc(value_len);
        if (!entry->key || !entry->value) {
            free(entry->key);
            free(entry->value);
            free(entry);
            pthread_mutex_unlock(&mem->lock);
            return -1;
        }
        memcpy(entry->key, key, key_len);
        entry->key_len = key_len;
        memcpy(entry->value, value, value_len);
        entry->value_len = value_len;

        HASH_ADD_KEYPTR(hh, mem->table, entry->key, entry->key_len, entry);
    }

    pthread_mutex_unlock(&mem->lock);
    return 0;
}

int kv_mem_retrieve(kv_mem_t *mem, const char *key, size_t key_len,
                    char **value, size_t *value_len)
{
    pthread_mutex_lock(&mem->lock);

    struct kv_entry *entry = NULL;
    HASH_FIND(hh, mem->table, key, key_len, entry);

    if (!entry) {
        pthread_mutex_unlock(&mem->lock);
        return -1;
    }

    *value = malloc(entry->value_len);
    if (!*value) {
        pthread_mutex_unlock(&mem->lock);
        return -1;
    }
    memcpy(*value, entry->value, entry->value_len);
    *value_len = entry->value_len;

    pthread_mutex_unlock(&mem->lock);
    return 0;
}

int kv_mem_delete(kv_mem_t *mem, const char *key, size_t key_len)
{
    pthread_mutex_lock(&mem->lock);

    struct kv_entry *entry = NULL;
    HASH_FIND(hh, mem->table, key, key_len, entry);

    if (entry) {
        HASH_DEL(mem->table, entry);
        free(entry->key);
        free(entry->value);
        free(entry);
    }
    /* 幂等: 不存在也返回成功 */

    pthread_mutex_unlock(&mem->lock);
    return 0;
}

int kv_mem_exist(kv_mem_t *mem, const char *key, size_t key_len)
{
    pthread_mutex_lock(&mem->lock);

    struct kv_entry *entry = NULL;
    HASH_FIND(hh, mem->table, key, key_len, entry);

    pthread_mutex_unlock(&mem->lock);
    return entry ? 1 : 0;
}

/* qsort 比较: 按 key 字节序排序 */
static int entry_cmp(const void *a, const void *b)
{
    const struct kv_mem_entry *ea = (const struct kv_mem_entry *)a;
    const struct kv_mem_entry *eb = (const struct kv_mem_entry *)b;
    size_t min_len = ea->key_len < eb->key_len ? ea->key_len : eb->key_len;
    int rc = memcmp(ea->key, eb->key, min_len);
    if (rc != 0)
        return rc;
    if (ea->key_len < eb->key_len) return -1;
    if (ea->key_len > eb->key_len) return 1;
    return 0;
}

struct kv_mem_list_result *kv_mem_list_prefix(kv_mem_t *mem,
                                              const char *prefix, size_t prefix_len)
{
    struct kv_mem_list_result *result = calloc(1, sizeof(*result));
    if (!result)
        return NULL;

    pthread_mutex_lock(&mem->lock);

    /* 第一遍: 计数 */
    size_t count = 0;
    struct kv_entry *cur, *tmp;
    HASH_ITER(hh, mem->table, cur, tmp) {
        if (cur->key_len >= prefix_len &&
            memcmp(cur->key, prefix, prefix_len) == 0) {
            count++;
        }
    }

    if (count == 0) {
        pthread_mutex_unlock(&mem->lock);
        return result;
    }

    result->entries = calloc(count, sizeof(result->entries[0]));
    if (!result->entries) {
        pthread_mutex_unlock(&mem->lock);
        free(result);
        return NULL;
    }

    /* 第二遍: 复制数据 */
    size_t idx = 0;
    HASH_ITER(hh, mem->table, cur, tmp) {
        if (cur->key_len >= prefix_len &&
            memcmp(cur->key, prefix, prefix_len) == 0) {
            struct kv_mem_entry *e = &result->entries[idx++];
            e->key = malloc(cur->key_len);
            e->value = malloc(cur->value_len);
            if (!e->key || !e->value) {
                /* 清理已分配的内容 */
                for (size_t j = 0; j <= idx - 1; j++) {
                    free(result->entries[j].key);
                    free(result->entries[j].value);
                }
                free(result->entries);
                free(result);
                pthread_mutex_unlock(&mem->lock);
                return NULL;
            }
            memcpy(e->key, cur->key, cur->key_len);
            e->key_len = cur->key_len;
            memcpy(e->value, cur->value, cur->value_len);
            e->value_len = cur->value_len;
        }
    }
    result->count = count;

    pthread_mutex_unlock(&mem->lock);

    /* 按 key 排序 */
    qsort(result->entries, result->count, sizeof(result->entries[0]), entry_cmp);

    return result;
}

void kv_mem_list_free(struct kv_mem_list_result *result)
{
    if (!result)
        return;
    for (size_t i = 0; i < result->count; i++) {
        free(result->entries[i].key);
        free(result->entries[i].value);
    }
    free(result->entries);
    free(result);
}
