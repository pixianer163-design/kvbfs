#include <stdio.h>
#include <stdarg.h>
#include <time.h>

/* 日志级别 */
enum log_level {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
};

static enum log_level g_log_level = LOG_INFO;

void log_set_level(int level)
{
    g_log_level = (enum log_level)level;
}

void log_msg(int level, const char *fmt, ...)
{
    if ((enum log_level)level < g_log_level) {
        return;
    }

    const char *level_str[] = {"DEBUG", "INFO", "WARN", "ERROR"};

    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&now, &tm_buf);

    fprintf(stderr, "[%04d-%02d-%02d %02d:%02d:%02d] [%s] ",
            tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
            tm->tm_hour, tm->tm_min, tm->tm_sec,
            level_str[level]);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fprintf(stderr, "\n");
}
