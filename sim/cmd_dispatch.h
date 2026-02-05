#ifndef CMD_DISPATCH_H
#define CMD_DISPATCH_H

#include "kv_mem.h"
#include "nvme_kv_proto.h"

/*
 * 命令分发 — 处理 NVMe KV 请求并生成响应
 *
 * dispatch 函数为纯函数 (仅依赖 kv_mem)，不涉及网络 I/O。
 * resp_data 由函数分配，调用方负责 free。
 */
void cmd_dispatch(kv_mem_t *mem,
                  const struct nvme_kv_req_hdr *req,
                  const char *key, const char *value,
                  struct nvme_kv_resp_hdr *resp,
                  char **resp_data, uint32_t *resp_data_len);

#endif /* CMD_DISPATCH_H */
