#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

/**
 * 信号量类
 * 封装了POSIX信号量，用于线程间的同步
 */
class sem
{
public:
    /**
     * 默认构造函数
     * 初始化信号量，初始值为0
     * @throw std::exception 初始化失败时抛出异常
     */
    sem()
    {
        if (sem_init(&m_sem, 0, 0) != 0)
        {
            throw std::exception();
        }
    }

    /**
     * 带参数构造函数
     * 初始化信号量，初始值为num
     * @param num 信号量初始值
     * @throw std::exception 初始化失败时抛出异常
     */
    sem(int num)
    {
        if (sem_init(&m_sem, 0, num) != 0)
        {
            throw std::exception();
        }
    }

    /**
     * 析构函数
     * 销毁信号量
     */
    ~sem()
    {
        sem_destroy(&m_sem);
    }

    /**
     * 等待信号量
     * 信号量值减1，若值为0则阻塞
     * @return 操作是否成功
     */
    bool wait()
    {
        return sem_wait(&m_sem) == 0;
    }

    /**
     * 发布信号量
     * 信号量值加1，唤醒等待的线程
     * @return 操作是否成功
     */
    bool post()
    {
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem; // 信号量对象
};

/**
 * 互斥锁类
 * 封装了POSIX互斥锁，用于线程间的互斥访问
 */
class locker
{
public:
    /**
     * 构造函数
     * 初始化互斥锁
     * @throw std::exception 初始化失败时抛出异常
     */
    locker()
    {
        if (pthread_mutex_init(&m_mutex, NULL) != 0)
        {
            throw std::exception();
        }
    }

    /**
     * 析构函数
     * 销毁互斥锁
     */
    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }

    /**
     * 加锁
     * 获取互斥锁，若被其他线程持有则阻塞
     * @return 操作是否成功
     */
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex) == 0;
    }

    /**
     * 解锁
     * 释放互斥锁
     * @return 操作是否成功
     */
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    /**
     * 获取互斥锁指针
     * @return 互斥锁指针
     */
    pthread_mutex_t *get()
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex; // 互斥锁对象
};

/**
 * 条件变量类
 * 封装了POSIX条件变量，用于线程间的条件等待
 */
class cond
{
public:
    /**
     * 构造函数
     * 初始化条件变量
     * @throw std::exception 初始化失败时抛出异常
     */
    cond()
    {
        if (pthread_cond_init(&m_cond, NULL) != 0)
        {
            // pthread_mutex_destroy(&m_mutex);
            throw std::exception();
        }
    }

    /**
     * 析构函数
     * 销毁条件变量
     */
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }

    /**
     * 等待条件变量
     * 释放互斥锁并阻塞等待，被唤醒后重新获取互斥锁
     * @param m_mutex 互斥锁指针
     * @return 操作是否成功
     */
    bool wait(pthread_mutex_t *m_mutex)
    {
        int ret = 0;
        // pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_wait(&m_cond, m_mutex);
        // pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }

    /**
     * 超时等待条件变量
     * 释放互斥锁并阻塞等待指定时间，被唤醒或超时后重新获取互斥锁
     * @param m_mutex 互斥锁指针
     * @param t 超时时间
     * @return 操作是否成功
     */
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t)
    {
        int ret = 0;
        // pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        // pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }

    /**
     * 发送信号
     * 唤醒一个等待该条件变量的线程
     * @return 操作是否成功
     */
    bool signal()
    {
        return pthread_cond_signal(&m_cond) == 0;
    }

    /**
     * 广播信号
     * 唤醒所有等待该条件变量的线程
     * @return 操作是否成功
     */
    bool broadcast()
    {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    // static pthread_mutex_t m_mutex;
    pthread_cond_t m_cond; // 条件变量对象
};

#endif