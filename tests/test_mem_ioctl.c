/*
 * test_mem_ioctl.c — CFS 记忆搜索 ioctl 测试工具
 *
 * 用法: test_mem_ioctl <session_file> <query_text> [top_k]
 * 输出: 每行一个结果，格式: score<TAB>ino:seq<TAB>summary
 * 退出码: 0=有结果, 1=无结果, 2=错误
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>

#define CFS_MEM_MAX_RESULTS  16
#define CFS_MEM_SUMMARY_LEN  512

struct cfs_mem_result {
    uint64_t ino;
    uint32_t seq;
    float    score;
    char     summary[CFS_MEM_SUMMARY_LEN];
};

struct cfs_mem_query {
    char     query_text[512];
    int      top_k;
    int      n_results;
    struct cfs_mem_result results[CFS_MEM_MAX_RESULTS];
};

#define CFS_IOC_MAGIC       'C'
#define CFS_IOC_MEM_SEARCH  _IOWR(CFS_IOC_MAGIC, 10, struct cfs_mem_query)

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "用法: %s <session_file> <query_text> [top_k]\n", argv[0]);
        return 2;
    }

    const char *path = argv[1];
    const char *query = argv[2];
    int top_k = argc > 3 ? atoi(argv[3]) : 5;

    if (top_k <= 0 || top_k > CFS_MEM_MAX_RESULTS)
        top_k = 5;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 2;
    }

    struct cfs_mem_query q;
    memset(&q, 0, sizeof(q));
    strncpy(q.query_text, query, sizeof(q.query_text) - 1);
    q.top_k = top_k;

    if (ioctl(fd, CFS_IOC_MEM_SEARCH, &q) < 0) {
        perror("ioctl CFS_IOC_MEM_SEARCH");
        close(fd);
        return 2;
    }

    close(fd);

    if (q.n_results <= 0) {
        fprintf(stderr, "无结果\n");
        return 1;
    }

    for (int i = 0; i < q.n_results; i++) {
        /* 截断 summary 中的换行 */
        for (char *p = q.results[i].summary; *p; p++) {
            if (*p == '\n' || *p == '\r') *p = ' ';
        }
        printf("%.4f\t%lu:%u\t%s\n",
               q.results[i].score,
               (unsigned long)q.results[i].ino,
               q.results[i].seq,
               q.results[i].summary);
    }

    return 0;
}
