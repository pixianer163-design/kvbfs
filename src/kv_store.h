#ifndef KV_STORE_H
#define KV_STORE_H

#include <stddef.h>
#include <stdint.h>

/* KV 存储抽象接口 */

/* 打开 KV 存储，返回句柄 */
void *kv_open(const char *path);

/* 关闭 KV 存储 */
void kv_close(void *db);

/* 读取值，返回值需要 free */
int kv_get(void *db, const char *key, size_t key_len,
           char **value, size_t *value_len);

/* 写入值 */
int kv_put(void *db, const char *key, size_t key_len,
           const char *value, size_t value_len);

/* 删除键 */
int kv_delete(void *db, const char *key, size_t key_len);

/* 前缀迭代器 */
typedef struct kv_iterator kv_iterator_t;

kv_iterator_t *kv_iter_prefix(void *db, const char *prefix, size_t prefix_len);
int kv_iter_valid(kv_iterator_t *iter);
void kv_iter_next(kv_iterator_t *iter);
const char *kv_iter_key(kv_iterator_t *iter, size_t *len);
const char *kv_iter_value(kv_iterator_t *iter, size_t *len);
void kv_iter_free(kv_iterator_t *iter);

#endif /* KV_STORE_H */
