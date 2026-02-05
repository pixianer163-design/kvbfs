#include "cmd_dispatch.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void init_resp(struct nvme_kv_resp_hdr *resp, uint32_t cmd_id)
{
    resp->magic     = NVME_KV_MAGIC;
    resp->status    = NVME_KV_SC_SUCCESS;
    resp->reserved  = 0;
    resp->value_len = 0;
    resp->cmd_id    = cmd_id;
}

static void handle_store(kv_mem_t *mem,
                          const struct nvme_kv_req_hdr *req,
                          const char *key, const char *value,
                          struct nvme_kv_resp_hdr *resp)
{
    if (req->key_len == 0 || req->key_len > NVME_KV_MAX_KEY_LEN) {
        resp->status = NVME_KV_SC_INVALID_KEY;
        return;
    }
    if (req->value_len > NVME_KV_MAX_VAL_LEN) {
        resp->status = NVME_KV_SC_INVALID_VALUE;
        return;
    }

    if (kv_mem_store(mem, key, req->key_len, value, req->value_len) != 0) {
        resp->status = NVME_KV_SC_INTERNAL_ERROR;
    }
}

static void handle_retrieve(kv_mem_t *mem,
                              const struct nvme_kv_req_hdr *req,
                              const char *key,
                              struct nvme_kv_resp_hdr *resp,
                              char **resp_data, uint32_t *resp_data_len)
{
    if (req->key_len == 0 || req->key_len > NVME_KV_MAX_KEY_LEN) {
        resp->status = NVME_KV_SC_INVALID_KEY;
        return;
    }

    char *val = NULL;
    size_t val_len = 0;
    if (kv_mem_retrieve(mem, key, req->key_len, &val, &val_len) != 0) {
        resp->status = NVME_KV_SC_NOT_FOUND;
        return;
    }

    *resp_data = val;
    *resp_data_len = (uint32_t)val_len;
    resp->value_len = (uint32_t)val_len;
}

static void handle_delete(kv_mem_t *mem,
                            const struct nvme_kv_req_hdr *req,
                            const char *key,
                            struct nvme_kv_resp_hdr *resp)
{
    if (req->key_len == 0 || req->key_len > NVME_KV_MAX_KEY_LEN) {
        resp->status = NVME_KV_SC_INVALID_KEY;
        return;
    }

    /* Delete 幂等 */
    kv_mem_delete(mem, key, req->key_len);
}

static void handle_exist(kv_mem_t *mem,
                           const struct nvme_kv_req_hdr *req,
                           const char *key,
                           struct nvme_kv_resp_hdr *resp)
{
    if (req->key_len == 0 || req->key_len > NVME_KV_MAX_KEY_LEN) {
        resp->status = NVME_KV_SC_INVALID_KEY;
        return;
    }

    if (!kv_mem_exist(mem, key, req->key_len)) {
        resp->status = NVME_KV_SC_NOT_FOUND;
    }
}

/*
 * List 响应数据格式:
 *   [uint16_t key_len][key bytes][uint32_t value_len][value bytes] ... (重复)
 */
static void handle_list(kv_mem_t *mem,
                          const struct nvme_kv_req_hdr *req,
                          const char *key,
                          struct nvme_kv_resp_hdr *resp,
                          char **resp_data, uint32_t *resp_data_len)
{
    /* key 作为前缀，长度可以为 0 (列出全部) */
    if (req->key_len > NVME_KV_MAX_KEY_LEN) {
        resp->status = NVME_KV_SC_INVALID_KEY;
        return;
    }

    struct kv_mem_list_result *result = kv_mem_list_prefix(mem, key, req->key_len);
    if (!result) {
        resp->status = NVME_KV_SC_INTERNAL_ERROR;
        return;
    }

    if (result->count == 0) {
        kv_mem_list_free(result);
        return;
    }

    /* 计算总大小 */
    size_t total = 0;
    for (size_t i = 0; i < result->count; i++) {
        total += sizeof(uint16_t) + result->entries[i].key_len +
                 sizeof(uint32_t) + result->entries[i].value_len;
    }

    char *buf = malloc(total);
    if (!buf) {
        kv_mem_list_free(result);
        resp->status = NVME_KV_SC_INTERNAL_ERROR;
        return;
    }

    /* 序列化 */
    size_t off = 0;
    for (size_t i = 0; i < result->count; i++) {
        uint16_t kl = (uint16_t)result->entries[i].key_len;
        uint32_t vl = (uint32_t)result->entries[i].value_len;

        memcpy(buf + off, &kl, sizeof(kl));
        off += sizeof(kl);
        memcpy(buf + off, result->entries[i].key, kl);
        off += kl;
        memcpy(buf + off, &vl, sizeof(vl));
        off += sizeof(vl);
        memcpy(buf + off, result->entries[i].value, vl);
        off += vl;
    }

    kv_mem_list_free(result);

    *resp_data = buf;
    *resp_data_len = (uint32_t)total;
    resp->value_len = (uint32_t)total;
}

void cmd_dispatch(kv_mem_t *mem,
                  const struct nvme_kv_req_hdr *req,
                  const char *key, const char *value,
                  struct nvme_kv_resp_hdr *resp,
                  char **resp_data, uint32_t *resp_data_len)
{
    init_resp(resp, req->cmd_id);
    *resp_data = NULL;
    *resp_data_len = 0;

    switch (req->opcode) {
    case NVME_KV_OP_STORE:
        handle_store(mem, req, key, value, resp);
        break;
    case NVME_KV_OP_RETRIEVE:
        handle_retrieve(mem, req, key, resp, resp_data, resp_data_len);
        break;
    case NVME_KV_OP_DELETE:
        handle_delete(mem, req, key, resp);
        break;
    case NVME_KV_OP_EXIST:
        handle_exist(mem, req, key, resp);
        break;
    case NVME_KV_OP_LIST:
        handle_list(mem, req, key, resp, resp_data, resp_data_len);
        break;
    default:
        fprintf(stderr, "sim: unknown opcode 0x%02x\n", req->opcode);
        resp->status = NVME_KV_SC_INTERNAL_ERROR;
        break;
    }
}
