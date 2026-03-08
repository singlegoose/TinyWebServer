#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
using namespace std;

/**
 * Log 构造函数
 * 初始化日志系统的基本状态
 */
Log::Log()
{
    m_count = 0;        // 日志计数
    m_is_async = false; // 默认使用同步模式
}

/**
 * Log 析构函数
 * 关闭日志文件
 */
Log::~Log()
{
    if (m_fp != NULL)
    {
        fclose(m_fp); // 关闭日志文件
    }
}

/**
 * 初始化日志系统
 * @param file_name 日志文件名
 * @param close_log 日志开关
 * @param log_buf_size 日志缓冲区大小
 * @param split_lines 日志文件分割行数
 * @param max_queue_size 异步队列大小
 * @return 初始化是否成功
 */
bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size)
{
    // 如果设置了max_queue_size,则设置为异步
    if (max_queue_size >= 1)
    {
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size); // 创建阻塞队列
        pthread_t tid;
        // flush_log_thread为回调函数,这里表示创建线程异步写日志
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }

    m_close_log = close_log;
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];    // 分配日志缓冲区
    memset(m_buf, '\0', m_log_buf_size); // 清空缓冲区
    m_split_lines = split_lines;

    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    // 解析日志文件名和路径
    const char *p = strrchr(file_name, '/');
    char log_full_name[256] = {0};

    if (p == NULL)
    {
        // 如果没有路径，直接使用文件名
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }
    else
    {
        // 提取文件名和路径
        strcpy(log_name, p + 1);
        strncpy(dir_name, file_name, p - file_name + 1);
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday; // 记录当前日期

    m_fp = fopen(log_full_name, "a"); // 以追加模式打开日志文件
    if (m_fp == NULL)
    {
        return false;
    }

    return true;
}

/**
 * 写入日志
 * @param level 日志级别
 * @param format 日志格式
 * @param ... 可变参数
 */
void Log::write_log(int level, const char *format, ...)
{
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL); // 获取当前时间
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16] = {0};
    // 根据日志级别设置前缀
    switch (level)
    {
    case 0:
        strcpy(s, "[debug]:");
        break;
    case 1:
        strcpy(s, "[info]:");
        break;
    case 2:
        strcpy(s, "[warn]:");
        break;
    case 3:
        strcpy(s, "[erro]:");
        break;
    default:
        strcpy(s, "[info]:");
        break;
    }
    // 写入一个log，对m_count++, m_split_lines最大行数
    m_mutex.lock(); // 加锁，保证线程安全
    m_count++;

    // 检查是否需要分割日志文件
    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) // everyday log
    {

        char new_log[256] = {0};
        fflush(m_fp); // 刷新缓冲区
        fclose(m_fp); // 关闭当前日志文件
        char tail[16] = {0};

        // 生成日期后缀
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);

        if (m_today != my_tm.tm_mday)
        {
            // 新的一天，创建新的日志文件
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        }
        else
        {
            // 日志文件达到最大行数，创建新的日志文件
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        m_fp = fopen(new_log, "a"); // 打开新的日志文件
    }

    m_mutex.unlock(); // 解锁

    va_list valst;
    va_start(valst, format); // 初始化可变参数

    string log_str;
    m_mutex.lock(); // 加锁，保证线程安全

    // 写入的具体时间内容格式
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);

    // 格式化日志内容
    int m = vsnprintf(m_buf + n, m_log_buf_size - n - 1, format, valst);
    m_buf[n + m] = '\n';     // 添加换行符
    m_buf[n + m + 1] = '\0'; // 添加结束符
    log_str = m_buf;

    m_mutex.unlock(); // 解锁

    // 根据模式选择写入方式
    if (m_is_async && !m_log_queue->full())
    {
        // 异步模式，将日志加入队列
        m_log_queue->push(log_str);
    }
    else
    {
        // 同步模式，直接写入文件
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst); // 结束可变参数处理
}

/**
 * 刷新日志缓冲区
 */
void Log::flush(void)
{
    m_mutex.lock();
    // 强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}