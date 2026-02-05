#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "nvme_kv_proto.h"
#include "cmd_dispatch.h"
#include "kv_mem.h"

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* 确保读取 n 个字节，返回 0 成功，-1 失败/断开 */
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

/* 确保发送 n 个字节 */
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

static int handle_connection(int client_fd, kv_mem_t *mem)
{
    printf("sim: client connected\n");

    while (g_running) {
        /* 接收请求头 */
        struct nvme_kv_req_hdr req;
        if (recv_exact(client_fd, &req, sizeof(req)) != 0)
            break;  /* 客户端断开 */

        if (req.magic != NVME_KV_MAGIC) {
            fprintf(stderr, "sim: bad magic 0x%08x\n", req.magic);
            break;
        }

        /* 接收 key */
        char key_buf[NVME_KV_MAX_KEY_LEN];
        if (req.key_len > 0) {
            if (req.key_len > NVME_KV_MAX_KEY_LEN) {
                fprintf(stderr, "sim: key too long %u\n", req.key_len);
                break;
            }
            if (recv_exact(client_fd, key_buf, req.key_len) != 0)
                break;
        }

        /* 接收 value (仅 Store) */
        char *value_buf = NULL;
        if (req.opcode == NVME_KV_OP_STORE && req.value_len > 0) {
            if (req.value_len > NVME_KV_MAX_VAL_LEN) {
                fprintf(stderr, "sim: value too large %u\n", req.value_len);
                break;
            }
            value_buf = malloc(req.value_len);
            if (!value_buf) {
                fprintf(stderr, "sim: out of memory\n");
                break;
            }
            if (recv_exact(client_fd, value_buf, req.value_len) != 0) {
                free(value_buf);
                break;
            }
        }

        /* 分发命令 */
        struct nvme_kv_resp_hdr resp;
        char *resp_data = NULL;
        uint32_t resp_data_len = 0;

        cmd_dispatch(mem, &req, key_buf, value_buf, &resp, &resp_data, &resp_data_len);

        free(value_buf);

        /* 发送响应头 */
        if (send_exact(client_fd, &resp, sizeof(resp)) != 0) {
            free(resp_data);
            break;
        }

        /* 发送响应数据 */
        if (resp_data_len > 0 && resp_data) {
            if (send_exact(client_fd, resp_data, resp_data_len) != 0) {
                free(resp_data);
                break;
            }
        }
        free(resp_data);
    }

    printf("sim: client disconnected\n");
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [--port PORT]\n", prog);
    fprintf(stderr, "  --port PORT   Listen port (default %d)\n", NVME_KV_DEFAULT_PORT);
}

int main(int argc, char *argv[])
{
    uint16_t port = NVME_KV_DEFAULT_PORT;

    static struct option long_opts[] = {
        {"port", required_argument, NULL, 'p'},
        {"help", no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p':
            port = (uint16_t)atoi(optarg);
            break;
        case 'h':
        default:
            usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    /* 信号处理 */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* 创建内存 KV 存储 */
    kv_mem_t *mem = kv_mem_create();
    if (!mem) {
        fprintf(stderr, "sim: failed to create kv store\n");
        return 1;
    }

    /* 创建 TCP 监听 socket */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("sim: socket");
        kv_mem_destroy(mem);
        return 1;
    }

    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("sim: bind");
        close(listen_fd);
        kv_mem_destroy(mem);
        return 1;
    }

    if (listen(listen_fd, 1) < 0) {
        perror("sim: listen");
        close(listen_fd);
        kv_mem_destroy(mem);
        return 1;
    }

    printf("nvme-kv-sim: listening on port %d\n", port);

    /* 接受连接循环 */
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("sim: accept");
            break;
        }

        handle_connection(client_fd, mem);
        close(client_fd);
    }

    printf("nvme-kv-sim: shutting down\n");
    close(listen_fd);
    kv_mem_destroy(mem);
    return 0;
}
