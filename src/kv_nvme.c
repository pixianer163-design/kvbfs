#include "kv_store.h"
#include "nvme_kv_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>

/* NVMe KV 连接句柄 */
struct nvme_kv_conn {
    int       sockfd;
    char      host[256];
    uint16_t  port;
    uint32_t  next_cmd_id;
    pthread_mutex_t send_lock;  /* 串行化请求/响应 */
};

/* 迭代器: 客户端缓存全部 List 结果 */
struct kv_iterator {
    struct iter_entry {
        char   *key;
        size_t  key_len;
        char   *value;
        size_t  value_len;
    } *entries;
    size_t count;
    size_t pos;
};

/* ---- 网络辅助函数 ---- */

static int recv_exact(int fd, void *buf, size_t n)
{
    size_t total = 0;
    while (total < n) {
        ssize_t r = recv(fd, (char *)buf + total, n - total, 0);
        if (r <= 0)
            return -1;
        total += (size_t)r;
    }
    return 0;
}

static int send_exact(int fd, const void *buf, size_t n)
{
    size_t total = 0;
    while (total < n) {
        ssize_t w = send(fd, (const char *)buf + total, n - total, 0);
        if (w <= 0)
            return -1;
        total += (size_t)w;
    }
    return 0;
}

/*
 * 通用事务函数: 发送请求 + 接收响应
 * resp_data 和 resp_data_len 可以为 NULL (不需要响应数据时)
 * 如果 resp_data 非 NULL，*resp_data 由函数分配，调用方 free
 */
static int nvme_kv_transact(struct nvme_kv_conn *conn,
                            uint8_t opcode, uint8_t flags,
                            const char *key, size_t key_len,
                            const char *value, size_t value_len,
                            struct nvme_kv_resp_hdr *resp,
                            char **resp_data, size_t *resp_data_len)
{
    pthread_mutex_lock(&conn->send_lock);

    /* 构造请求头 */
    struct nvme_kv_req_hdr req;
    memset(&req, 0, sizeof(req));
    req.magic     = NVME_KV_MAGIC;
    req.version   = NVME_KV_VERSION;
    req.opcode    = opcode;
    req.flags     = flags;
    req.key_len   = (uint16_t)key_len;
    req.value_len = (opcode == NVME_KV_OP_STORE) ? (uint32_t)value_len : 0;
    req.cmd_id    = conn->next_cmd_id++;

    int rc = -1;

    /* 发送请求头 */
    if (send_exact(conn->sockfd, &req, sizeof(req)) != 0)
        goto out;

    /* 发送 key */
    if (key_len > 0) {
        if (send_exact(conn->sockfd, key, key_len) != 0)
            goto out;
    }

    /* 发送 value (仅 Store) */
    if (opcode == NVME_KV_OP_STORE && value_len > 0) {
        if (send_exact(conn->sockfd, value, value_len) != 0)
            goto out;
    }

    /* 接收响应头 */
    if (recv_exact(conn->sockfd, resp, sizeof(*resp)) != 0)
        goto out;

    if (resp->magic != NVME_KV_MAGIC) {
        fprintf(stderr, "kv_nvme: bad response magic 0x%08x\n", resp->magic);
        goto out;
    }

    /* 接收响应数据 */
    if (resp->value_len > 0) {
        char *data = malloc(resp->value_len);
        if (!data)
            goto out;
        if (recv_exact(conn->sockfd, data, resp->value_len) != 0) {
            free(data);
            goto out;
        }
        if (resp_data) {
            *resp_data = data;
            if (resp_data_len)
                *resp_data_len = resp->value_len;
        } else {
            free(data);
        }
    } else {
        if (resp_data)
            *resp_data = NULL;
        if (resp_data_len)
            *resp_data_len = 0;
    }

    rc = 0;
out:
    pthread_mutex_unlock(&conn->send_lock);
    return rc;
}

/* ---- kv_store.h 接口实现 ---- */

void *kv_open(const char *path)
{
    struct nvme_kv_conn *conn = calloc(1, sizeof(*conn));
    if (!conn)
        return NULL;

    /* 解析 "host:port" */
    const char *colon = strrchr(path, ':');
    if (!colon) {
        /* 仅 host，使用默认端口 */
        snprintf(conn->host, sizeof(conn->host), "%s", path);
        conn->port = NVME_KV_DEFAULT_PORT;
    } else {
        size_t host_len = (size_t)(colon - path);
        if (host_len >= sizeof(conn->host))
            host_len = sizeof(conn->host) - 1;
        memcpy(conn->host, path, host_len);
        conn->host[host_len] = '\0';
        conn->port = (uint16_t)atoi(colon + 1);
    }

    pthread_mutex_init(&conn->send_lock, NULL);

    /* TCP 连接 */
    conn->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (conn->sockfd < 0) {
        perror("kv_nvme: socket");
        free(conn);
        return NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(conn->port);

    if (inet_pton(AF_INET, conn->host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "kv_nvme: invalid address '%s'\n", conn->host);
        close(conn->sockfd);
        free(conn);
        return NULL;
    }

    if (connect(conn->sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "kv_nvme: connect to %s:%d failed: %s\n",
                conn->host, conn->port, strerror(errno));
        close(conn->sockfd);
        free(conn);
        return NULL;
    }

    printf("kv_nvme: connected to %s:%d\n", conn->host, conn->port);
    return conn;
}

void kv_close(void *db)
{
    struct nvme_kv_conn *conn = (struct nvme_kv_conn *)db;
    if (!conn)
        return;
    close(conn->sockfd);
    pthread_mutex_destroy(&conn->send_lock);
    free(conn);
}

int kv_get(void *db, const char *key, size_t key_len,
           char **value, size_t *value_len)
{
    struct nvme_kv_conn *conn = (struct nvme_kv_conn *)db;
    struct nvme_kv_resp_hdr resp;

    if (nvme_kv_transact(conn, NVME_KV_OP_RETRIEVE, 0,
                         key, key_len, NULL, 0,
                         &resp, value, value_len) != 0) {
        return -1;
    }

    if (resp.status != NVME_KV_SC_SUCCESS)
        return -1;

    return 0;
}

int kv_put(void *db, const char *key, size_t key_len,
           const char *value, size_t value_len)
{
    struct nvme_kv_conn *conn = (struct nvme_kv_conn *)db;
    struct nvme_kv_resp_hdr resp;

    if (nvme_kv_transact(conn, NVME_KV_OP_STORE, 0,
                         key, key_len, value, value_len,
                         &resp, NULL, NULL) != 0) {
        return -1;
    }

    if (resp.status != NVME_KV_SC_SUCCESS)
        return -1;

    return 0;
}

int kv_delete(void *db, const char *key, size_t key_len)
{
    struct nvme_kv_conn *conn = (struct nvme_kv_conn *)db;
    struct nvme_kv_resp_hdr resp;

    if (nvme_kv_transact(conn, NVME_KV_OP_DELETE, 0,
                         key, key_len, NULL, 0,
                         &resp, NULL, NULL) != 0) {
        return -1;
    }

    if (resp.status != NVME_KV_SC_SUCCESS)
        return -1;

    return 0;
}

kv_iterator_t *kv_iter_prefix(void *db, const char *prefix, size_t prefix_len)
{
    struct nvme_kv_conn *conn = (struct nvme_kv_conn *)db;

    kv_iterator_t *iter = calloc(1, sizeof(*iter));
    if (!iter)
        return NULL;

    /* 发送 List 请求 */
    struct nvme_kv_resp_hdr resp;
    char *data = NULL;
    size_t data_len = 0;

    if (nvme_kv_transact(conn, NVME_KV_OP_LIST, 0,
                         prefix, prefix_len, NULL, 0,
                         &resp, &data, &data_len) != 0) {
        free(iter);
        return NULL;
    }

    if (resp.status != NVME_KV_SC_SUCCESS || data_len == 0) {
        free(data);
        /* 返回空迭代器 */
        return iter;
    }

    /* 解析 List 响应: [uint16_t key_len][key][uint32_t val_len][val] ... */
    /* 先数有多少个条目 */
    size_t count = 0;
    size_t off = 0;
    while (off < data_len) {
        if (off + sizeof(uint16_t) > data_len) break;
        uint16_t kl;
        memcpy(&kl, data + off, sizeof(kl));
        off += sizeof(kl) + kl;

        if (off + sizeof(uint32_t) > data_len) break;
        uint32_t vl;
        memcpy(&vl, data + off, sizeof(vl));
        off += sizeof(vl) + vl;

        count++;
    }

    if (count == 0) {
        free(data);
        return iter;
    }

    iter->entries = calloc(count, sizeof(iter->entries[0]));
    if (!iter->entries) {
        free(data);
        free(iter);
        return NULL;
    }
    iter->count = count;

    /* 第二遍: 复制数据 */
    off = 0;
    for (size_t i = 0; i < count; i++) {
        uint16_t kl;
        memcpy(&kl, data + off, sizeof(kl));
        off += sizeof(kl);

        iter->entries[i].key = malloc(kl);
        if (iter->entries[i].key)
            memcpy(iter->entries[i].key, data + off, kl);
        iter->entries[i].key_len = kl;
        off += kl;

        uint32_t vl;
        memcpy(&vl, data + off, sizeof(vl));
        off += sizeof(vl);

        iter->entries[i].value = malloc(vl);
        if (iter->entries[i].value)
            memcpy(iter->entries[i].value, data + off, vl);
        iter->entries[i].value_len = vl;
        off += vl;
    }

    free(data);
    return iter;
}

int kv_iter_valid(kv_iterator_t *iter)
{
    if (!iter)
        return 0;
    return iter->pos < iter->count;
}

void kv_iter_next(kv_iterator_t *iter)
{
    if (iter && iter->pos < iter->count)
        iter->pos++;
}

const char *kv_iter_key(kv_iterator_t *iter, size_t *len)
{
    if (!iter || iter->pos >= iter->count)
        return NULL;
    if (len)
        *len = iter->entries[iter->pos].key_len;
    return iter->entries[iter->pos].key;
}

const char *kv_iter_value(kv_iterator_t *iter, size_t *len)
{
    if (!iter || iter->pos >= iter->count)
        return NULL;
    if (len)
        *len = iter->entries[iter->pos].value_len;
    return iter->entries[iter->pos].value;
}

void kv_iter_free(kv_iterator_t *iter)
{
    if (!iter)
        return;
    for (size_t i = 0; i < iter->count; i++) {
        free(iter->entries[i].key);
        free(iter->entries[i].value);
    }
    free(iter->entries);
    free(iter);
}
