#include "kv_store.h"

#include <stdlib.h>
#include <string.h>
#include <rocksdb/c.h>

struct kv_iterator {
    rocksdb_iterator_t *iter;
    char *prefix;
    size_t prefix_len;
};

void *kv_open(const char *path)
{
    rocksdb_options_t *options = rocksdb_options_create();
    rocksdb_options_set_create_if_missing(options, 1);

    char *err = NULL;
    rocksdb_t *db = rocksdb_open(options, path, &err);
    rocksdb_options_destroy(options);

    if (err) {
        free(err);
        return NULL;
    }
    return db;
}

void kv_close(void *db)
{
    if (db) {
        rocksdb_close((rocksdb_t *)db);
    }
}

int kv_get(void *db, const char *key, size_t key_len,
           char **value, size_t *value_len)
{
    rocksdb_readoptions_t *opts = rocksdb_readoptions_create();
    char *err = NULL;

    *value = rocksdb_get((rocksdb_t *)db, opts, key, key_len, value_len, &err);
    rocksdb_readoptions_destroy(opts);

    if (err) {
        free(err);
        return -1;
    }
    if (*value == NULL) {
        return -1;  /* key not found */
    }
    return 0;
}

int kv_put(void *db, const char *key, size_t key_len,
           const char *value, size_t value_len)
{
    rocksdb_writeoptions_t *opts = rocksdb_writeoptions_create();
    char *err = NULL;

    rocksdb_put((rocksdb_t *)db, opts, key, key_len, value, value_len, &err);
    rocksdb_writeoptions_destroy(opts);

    if (err) {
        free(err);
        return -1;
    }
    return 0;
}

int kv_delete(void *db, const char *key, size_t key_len)
{
    rocksdb_writeoptions_t *opts = rocksdb_writeoptions_create();
    char *err = NULL;

    rocksdb_delete((rocksdb_t *)db, opts, key, key_len, &err);
    rocksdb_writeoptions_destroy(opts);

    if (err) {
        free(err);
        return -1;
    }
    return 0;
}

kv_iterator_t *kv_iter_prefix(void *db, const char *prefix, size_t prefix_len)
{
    kv_iterator_t *iter = malloc(sizeof(kv_iterator_t));
    if (!iter) return NULL;

    iter->prefix = malloc(prefix_len);
    if (!iter->prefix) {
        free(iter);
        return NULL;
    }
    memcpy(iter->prefix, prefix, prefix_len);
    iter->prefix_len = prefix_len;

    rocksdb_readoptions_t *opts = rocksdb_readoptions_create();
    iter->iter = rocksdb_create_iterator((rocksdb_t *)db, opts);
    rocksdb_readoptions_destroy(opts);

    rocksdb_iter_seek(iter->iter, prefix, prefix_len);
    return iter;
}

int kv_iter_valid(kv_iterator_t *iter)
{
    if (!rocksdb_iter_valid(iter->iter)) {
        return 0;
    }

    size_t key_len;
    const char *key = rocksdb_iter_key(iter->iter, &key_len);

    if (key_len < iter->prefix_len) {
        return 0;
    }
    return memcmp(key, iter->prefix, iter->prefix_len) == 0;
}

void kv_iter_next(kv_iterator_t *iter)
{
    rocksdb_iter_next(iter->iter);
}

const char *kv_iter_key(kv_iterator_t *iter, size_t *len)
{
    return rocksdb_iter_key(iter->iter, len);
}

const char *kv_iter_value(kv_iterator_t *iter, size_t *len)
{
    return rocksdb_iter_value(iter->iter, len);
}

void kv_iter_free(kv_iterator_t *iter)
{
    if (iter) {
        rocksdb_iter_destroy(iter->iter);
        free(iter->prefix);
        free(iter);
    }
}
