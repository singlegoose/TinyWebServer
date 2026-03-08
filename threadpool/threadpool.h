#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

/**
 * 线程池模板类
 * 用于处理并发请求，支持 Reactor 和 Proactor 两种模式
 * @tparam T 任务类型，通常为 HTTP 连接
 */
template <typename T>
class threadpool
{
public:
    /**
     * 构造函数
     * @param actor_model 反应堆模型（0:Proactor, 1:Reactor）
     * @param connPool 数据库连接池
     * @param thread_number 线程池中线程的数量，默认8
     * @param max_request 请求队列中最多允许的、等待处理的请求的数量，默认10000
     * @throw std::exception 初始化失败时抛出异常
     */
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);

    /**
     * 析构函数
     * 释放线程池资源
     */
    ~threadpool();

    /**
     * 向请求队列中添加任务（Reactor模式）
     * @param request 任务指针
     * @param state 任务状态（0:读, 1:写）
     * @return 添加是否成功
     */
    bool append(T *request, int state);

    /**
     * 向请求队列中添加任务（Proactor模式）
     * @param request 任务指针
     * @return 添加是否成功
     */
    bool append_p(T *request);

private:
    /**
     * 工作线程运行的函数
     * 它不断从工作队列中取出任务并执行之
     * @param arg 线程池指针
     * @return 线程返回值
     */
    static void *worker(void *arg);

    /**
     * 线程运行的核心函数
     * 处理队列中的任务
     */
    void run();

private:
    int m_thread_number;         // 线程池中的线程数
    int m_max_requests;          // 请求队列中允许的最大请求数
    pthread_t *m_threads;        // 描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue;  // 请求队列
    locker m_queuelocker;        // 保护请求队列的互斥锁
    sem m_queuestat;             // 是否有任务需要处理的信号量
    connection_pool *m_connPool; // 数据库连接池
    int m_actor_model;           // 模型切换（0:Proactor, 1:Reactor）
};

/**
 * 构造函数实现
 * @tparam T 任务类型，通常为 HTTP 连接
 * @param actor_model 反应堆模型（0:Proactor, 1:Reactor）
 * @param connPool 数据库连接池
 * @param thread_number 线程池中线程的数量
 * @param max_requests 请求队列中允许的最大请求数
 * @throw std::exception 初始化失败时抛出异常
 */
template <typename T>
threadpool<T>::threadpool(int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model),     // 初始化反应堆模型
                                                                                                             m_thread_number(thread_number), // 初始化线程数量
                                                                                                             m_max_requests(max_requests),   // 初始化最大请求数
                                                                                                             m_threads(NULL),                // 初始化线程数组指针为 NULL
                                                                                                             m_connPool(connPool)            // 初始化数据库连接池
{
    // 检查参数有效性
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();

    // 分配线程数组，大小为 thread_number
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();

    // 创建线程
    for (int i = 0; i < thread_number; ++i)
    {
        // 创建线程，入口函数为 worker，参数为 this
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            // 创建失败，释放线程数组并抛出异常
            delete[] m_threads;
            throw std::exception();
        }
        // 分离线程，使其自动回收资源，避免内存泄漏
        if (pthread_detach(m_threads[i]))
        {
            // 分离失败，释放线程数组并抛出异常
            delete[] m_threads;
            throw std::exception();
        }
    }
}

/**
 * 析构函数实现
 */
template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}

/**
 * 向请求队列中添加任务（Reactor模式）
 */
template <typename T>
bool threadpool<T>::append(T *request, int state)
{
    m_queuelocker.lock(); // 加锁，保证线程安全

    // 检查队列是否已满
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }

    // 设置任务状态并添加到队列
    request->m_state = state;
    m_workqueue.push_back(request);

    m_queuelocker.unlock(); // 解锁
    m_queuestat.post();     // 信号量加1，通知等待的线程
    return true;
}

/**
 * 向请求队列中添加任务（Proactor模式）
 */
template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock(); // 加锁，保证线程安全

    // 检查队列是否已满
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }

    // 添加任务到队列
    m_workqueue.push_back(request);

    m_queuelocker.unlock(); // 解锁
    m_queuestat.post();     // 信号量加1，通知等待的线程
    return true;
}

/**
 * 工作线程函数
 */
template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run(); // 调用线程池的run方法
    return pool;
}
/**
 * 线程运行的核心函数
 * 工作线程的主循环，不断从队列中取出任务并执行
 * @tparam T 任务类型，通常为 HTTP 连接
 */
template <typename T>
void threadpool<T>::run()
{
    // 无限循环，持续处理任务
    while (true)
    {
        m_queuestat.wait(); // 等待任务信号，队列为空时阻塞

        m_queuelocker.lock(); // 加锁，保证线程安全，防止多个线程同时操作队列

        // 检查队列是否为空（双重检查，避免信号量虚假唤醒）
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock(); // 队列为空，解锁并继续等待
            continue;
        }

        // 取出队列中的第一个任务
        T *request = m_workqueue.front();
        m_workqueue.pop_front(); // 从队列中移除任务

        m_queuelocker.unlock(); // 解锁，允许其他线程操作队列

        // 检查任务是否有效
        if (!request)
            continue;

        // 根据模型类型处理任务
        if (1 == m_actor_model) // Reactor模式
        {
            if (0 == request->m_state) // 读状态
            {
                if (request->read_once()) // 读取数据
                {
                    request->improv = 1;                                  // 标记任务已处理
                    connectionRAII mysqlcon(&request->mysql, m_connPool); // 自动管理数据库连接
                    request->process();                                   // 处理请求
                }
                else
                {
                    request->improv = 1;     // 标记任务已处理
                    request->timer_flag = 1; // 标记为超时，需要关闭连接
                }
            }
            else // 写状态
            {
                if (request->write()) // 写入数据
                {
                    request->improv = 1; // 标记任务已处理
                }
                else
                {
                    request->improv = 1;     // 标记任务已处理
                    request->timer_flag = 1; // 标记为超时，需要关闭连接
                }
            }
        }
        else // Proactor模式
        {
            connectionRAII mysqlcon(&request->mysql, m_connPool); // 自动管理数据库连接
            request->process();                                   // 处理请求，Proactor模式下读写操作由主线程完成
        }
    }
}

#endif