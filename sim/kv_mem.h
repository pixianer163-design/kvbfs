#ifndef KV_MEM_H
#define KV_MEM_H

#include <stddef.h>
#include <stdint.h>

/*
 * 内存 KV 存储 — 基于 uthash 的哈希表
 * 支持二进制 key，线程安全
 */

typedef struct kv_mem kv_mem_t;

/* 前缀查询结果条目 */
struct kv_mem_entry {
    char    *key;
    size_t   key_len;
    char    *value;
    size_t   value_len;
};

/* 前缀查询结果集 */
struct kv_mem_list_result {
    struct kv_mem_entry *entries;
    size_t               count;
};

/* 创建/销毁 */
kv_mem_t *kv_mem_create(void);
void      kv_mem_destroy(kv_mem_t *mem);

/* 存储: 成功返回 0 */
int kv_mem_store(kv_mem_t *mem, const char *key, size_t key_len,
                 const char *value, size_t value_len);

/* 读取: 成功返回 0，*value 和 *value_len 由调用方 free */
int kv_mem_retrieve(kv_mem_t *mem, const char *key, size_t key_len,
                    char **value, size_t *value_len);

/* 删除: 成功返回 0，不存在也返回 0 (幂等) */
int kv_mem_delete(kv_mem_t *mem, const char *key, size_t key_len);

/* 判断 key 是否存在: 存在返回 1，不存在返回 0 */
int kv_mem_exist(kv_mem_t *mem, const char *key, size_t key_len);

/* 前缀列表: 返回结果集，需调用 kv_mem_list_free 释放 */
struct kv_mem_list_result *kv_mem_list_prefix(kv_mem_t *mem,
                                              const char *prefix, size_t prefix_len);

/* 释放前缀列表结果 */
void kv_mem_list_free(struct kv_mem_list_result *result);

#endif /* KV_MEM_H */
