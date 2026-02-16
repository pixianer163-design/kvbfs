#include <stdio.h>
#include <assert.h>

#include "../src/kvbfs.h"
#include "../src/inode.h"

/* 测试程序中定义全局上下文（主程序中在 main.c 定义） */
struct kvbfs_ctx *g_ctx = NULL;

int main(void)
{
    printf("Testing inode management...\n");

    /* TODO: 添加测试用例 */

    printf("All tests passed!\n");
    return 0;
}
