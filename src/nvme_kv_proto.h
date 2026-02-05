#ifndef NVME_KV_PROTO_H
#define NVME_KV_PROTO_H

#include <stdint.h>

/*
 * NVMe KV 命令协议定义
 * 共享于模拟器 (sim/) 和客户端 (kv_nvme.c)
 */

/* 协议魔数 "NVKV" */
#define NVME_KV_MAGIC 0x4E564B56

/* 协议版本 */
#define NVME_KV_VERSION 1

/* NVMe KV 操作码 (与规范一致) */
#define NVME_KV_OP_STORE    0x01
#define NVME_KV_OP_RETRIEVE 0x02
#define NVME_KV_OP_LIST     0x06
#define NVME_KV_OP_DELETE   0x10
#define NVME_KV_OP_EXIST    0x14

/* 状态码 */
#define NVME_KV_SC_SUCCESS        0x0000
#define NVME_KV_SC_NOT_FOUND      0x0001
#define NVME_KV_SC_EXISTS         0x0002
#define NVME_KV_SC_INVALID_KEY    0x0003
#define NVME_KV_SC_INVALID_VALUE  0x0004
#define NVME_KV_SC_INTERNAL_ERROR 0x00FF

/* 常量 */
#define NVME_KV_MAX_KEY_LEN (272)
#define NVME_KV_MAX_VAL_LEN (2 * 1024 * 1024) /* 2 MB */

/* 默认端口 */
#define NVME_KV_DEFAULT_PORT 9527

/* 请求头 (24 字节) */
struct nvme_kv_req_hdr {
    uint32_t magic;       /* NVME_KV_MAGIC */
    uint8_t  version;     /* 协议版本 = 1 */
    uint8_t  opcode;      /* NVMe KV opcode */
    uint8_t  flags;       /* 条件写标志等 */
    uint8_t  reserved;
    uint16_t key_len;     /* 0-272 */
    uint16_t reserved2;
    uint32_t value_len;   /* Store 时的 value 长度 */
    uint32_t cmd_id;      /* 命令 ID，响应中回传 */
    uint32_t reserved3;
};

/* 响应头 (16 字节) */
struct nvme_kv_resp_hdr {
    uint32_t magic;       /* NVME_KV_MAGIC */
    uint16_t status;      /* 状态码 */
    uint16_t reserved;
    uint32_t value_len;   /* 响应数据长度 */
    uint32_t cmd_id;      /* 回传命令 ID */
};

/*
 * 完整请求: [req_hdr][key (key_len bytes)][value (value_len bytes, 仅 Store)]
 * 完整响应: [resp_hdr][value (value_len bytes, 仅 Retrieve/List)]
 *
 * List 响应数据格式:
 *   [uint16_t key_len][key bytes][uint32_t value_len][value bytes] ... (重复)
 */

#endif /* NVME_KV_PROTO_H */
