/*************************************************************
 *循环数组实现的阻塞队列，m_back = (m_back + 1) % m_max_size;
 *线程安全，每个操作前都要先加互斥锁，操作完后，再解锁
 **************************************************************/

#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include "../lock/locker.h"
using namespace std;

/**
 * 阻塞队列类
 * 基于循环数组实现，线程安全，支持阻塞操作
 * @tparam T 队列元素类型
 */
template <class T>
class block_queue
{
public:
    /**
     * 构造函数
     * @param max_size 队列最大容量，默认1000
     */
    block_queue(int max_size = 1000)
    {
        if (max_size <= 0)
        {
            exit(-1);
        }

        m_max_size = max_size;
        m_array = new T[max_size];
        m_size = 0;
        m_front = -1;
        m_back = -1;
    }

    /**
     * 清空队列
     */
    void clear()
    {
        m_mutex.lock();
        m_size = 0;
        m_front = -1;
        m_back = -1;
        m_mutex.unlock();
    }

    /**
     * 析构函数
     */
    ~block_queue()
    {
        m_mutex.lock();
        if (m_array != NULL)
            delete[] m_array;

        m_mutex.unlock();
    }

    /**
     * 判断队列是否满了
     * @return 队列是否满
     */
    bool full()
    {
        m_mutex.lock();
        if (m_size >= m_max_size)
        {

            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    /**
     * 判断队列是否为空
     * @return 队列是否为空
     */
    bool empty()
    {
        m_mutex.lock();
        if (0 == m_size)
        {
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    /**
     * 返回队首元素
     * @param value 用于存储队首元素的引用
     * @return 操作是否成功
     */
    bool front(T &value)
    {
        m_mutex.lock();
        if (0 == m_size)
        {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_front];
        m_mutex.unlock();
        return true;
    }

    /**
     * 返回队尾元素
     * @param value 用于存储队尾元素的引用
     * @return 操作是否成功
     */
    bool back(T &value)
    {
        m_mutex.lock();
        if (0 == m_size)
        {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_back];
        m_mutex.unlock();
        return true;
    }

    /**
     * 获取队列当前大小
     * @return 队列大小
     */
    int size()
    {
        int tmp = 0;

        m_mutex.lock();
        tmp = m_size;

        m_mutex.unlock();
        return tmp;
    }

    /**
     * 获取队列最大容量
     * @return 队列最大容量
     */
    int max_size()
    {
        int tmp = 0;

        m_mutex.lock();
        tmp = m_max_size;

        m_mutex.unlock();
        return tmp;
    }

    /**
     * 往队列添加元素
     * 当队列满时，会唤醒所有等待的线程
     * @param item 要添加的元素
     * @return 操作是否成功
     */
    bool push(const T &item)
    {

        m_mutex.lock();
        if (m_size >= m_max_size)
        {

            m_cond.broadcast();
            m_mutex.unlock();
            return false;
        }

        m_back = (m_back + 1) % m_max_size;
        m_array[m_back] = item;

        m_size++;

        m_cond.broadcast();
        m_mutex.unlock();
        return true;
    }

    /**
     * 从队列取出元素
     * 当队列空时，会阻塞等待条件变量
     * @param item 用于存储取出元素的引用
     * @return 操作是否成功
     */
    bool pop(T &item)
    {

        m_mutex.lock();
        while (m_size <= 0)
        {

            if (!m_cond.wait(m_mutex.get()))
            {
                m_mutex.unlock();
                return false;
            }
        }

        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }

    /**
     * 从队列取出元素（带超时处理）
     * 当队列空时，会阻塞等待指定时间
     * @param item 用于存储取出元素的引用
     * @param ms_timeout 超时时间（毫秒）
     * @return 操作是否成功
     */
    bool pop(T &item, int ms_timeout)
    {
        struct timespec t = {0, 0};
        struct timeval now = {0, 0};
        gettimeofday(&now, NULL);
        m_mutex.lock();
        if (m_size <= 0)
        {
            t.tv_sec = now.tv_sec + ms_timeout / 1000;
            t.tv_nsec = (ms_timeout % 1000) * 1000;
            if (!m_cond.timewait(m_mutex.get(), t))
            {
                m_mutex.unlock();
                return false;
            }
        }

        if (m_size <= 0)
        {
            m_mutex.unlock();
            return false;
        }

        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }

private:
    locker m_mutex; // 互斥锁，保证线程安全
    cond m_cond;    // 条件变量，用于阻塞和唤醒线程

    T *m_array;     // 循环数组，存储队列元素
    int m_size;     // 当前队列大小
    int m_max_size; // 队列最大容量
    int m_front;    // 队首索引
    int m_back;     // 队尾索引
};

#endif