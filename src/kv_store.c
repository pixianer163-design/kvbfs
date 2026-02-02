#include "kv_store.h"

/*
 * KV 存储抽象层
 * 当前实现使用 RocksDB (kv_rocksdb.c)
 * 未来可替换为 NVMe KV 接口
 */
