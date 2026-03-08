#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"

using namespace std;

/**
 * 日志类
 * 实现了同步和异步两种日志写入模式，支持日志文件自动分割
 */
class Log
{
public:
    /**
     * 获取日志实例（单例模式）
     * C++11以后,使用局部变量懒汉不用加锁
     * @return 日志实例指针
     */
    static Log *get_instance()
    {
        static Log instance;
        return &instance;
    }

    /**
     * 刷新日志线程函数
     * @param args 线程参数
     * @return 线程返回值
     */
    static void *flush_log_thread(void *args)
    {
        Log::get_instance()->async_write_log();
    }

    /**
     * 初始化日志系统
     * @param file_name 日志文件名
     * @param close_log 日志开关
     * @param log_buf_size 日志缓冲区大小，默认8192
     * @param split_lines 日志文件最大行数，默认5000000
     * @param max_queue_size 异步队列大小，默认0（同步模式）
     * @return 初始化是否成功
     */
    bool init(const char *file_name, int close_log, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

    /**
     * 写入日志
     * @param level 日志级别
     * @param format 日志格式
     * @param ... 可变参数
     */
    void write_log(int level, const char *format, ...);

    /**
     * 刷新日志缓冲区
     */
    void flush(void);

private:
    /**
     * 构造函数（私有，单例模式）
     */
    Log();

    /**
     * 析构函数（虚函数，确保子类正确析构）
     */
    virtual ~Log();

    /**
     * 异步写入日志
     * 从阻塞队列中取出日志并写入文件
     * @return 线程返回值
     */
    void *async_write_log()
    {
        string single_log;
        // 从阻塞队列中取出一个日志string，写入文件
        while (m_log_queue->pop(single_log))
        {
            m_mutex.lock();
            fputs(single_log.c_str(), m_fp);
            m_mutex.unlock();
        }
    }

private:
    char dir_name[128];               // 路径名
    char log_name[128];               // log文件名
    int m_split_lines;                // 日志最大行数
    int m_log_buf_size;               // 日志缓冲区大小
    long long m_count;                // 日志行数记录
    int m_today;                      // 因为按天分类,记录当前时间是那一天
    FILE *m_fp;                       // 打开log的文件指针
    char *m_buf;                      // 日志缓冲区
    block_queue<string> *m_log_queue; // 阻塞队列，用于异步日志
    bool m_is_async;                  // 是否同步标志位
    locker m_mutex;                   // 互斥锁，保证线程安全
    int m_close_log;                  // 关闭日志
};

/**
 * 日志宏定义
 * 不同级别的日志写入宏
 */
#define LOG_DEBUG(format, ...)                                    \
    if (0 == m_close_log)                                         \
    {                                                             \
        Log::get_instance()->write_log(0, format, ##__VA_ARGS__); \
        Log::get_instance()->flush();                             \
    }
#define LOG_INFO(format, ...)                                     \
    if (0 == m_close_log)                                         \
    {                                                             \
        Log::get_instance()->write_log(1, format, ##__VA_ARGS__); \
        Log::get_instance()->flush();                             \
    }
#define LOG_WARN(format, ...)                                     \
    if (0 == m_close_log)                                         \
    {                                                             \
        Log::get_instance()->write_log(2, format, ##__VA_ARGS__); \
        Log::get_instance()->flush();                             \
    }
#define LOG_ERROR(format, ...)                                    \
    if (0 == m_close_log)                                         \
    {                                                             \
        Log::get_instance()->write_log(3, format, ##__VA_ARGS__); \
        Log::get_instance()->flush();                             \
    }

#endif