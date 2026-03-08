#include "lst_timer.h"
#include "../http/http_conn.h"

/**
 * sort_timer_lst 构造函数
 * 初始化定时器链表的头尾指针为 NULL
 */
sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}

/**
 * sort_timer_lst 析构函数
 * 释放链表中所有定时器的内存
 */
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

/**
 * 添加定时器到链表
 * @param timer 要添加的定时器
 */
void sort_timer_lst::add_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if (!head)
    {
        // 链表为空，直接作为头节点
        head = tail = timer;
        return;
    }
    if (timer->expire < head->expire)
    {
        // 新定时器过期时间早于头节点，作为新的头节点
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    // 否则调用内部方法添加到合适位置
    add_timer(timer, head);
}

/**
 * 调整定时器在链表中的位置
 * @param timer 要调整的定时器
 */
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;
    // 如果定时器在链表尾部或其过期时间仍早于下一个定时器，则不需要调整
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }
    if (timer == head)
    {
        // 如果是头节点，将其移到链表中合适位置
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    else
    {
        // 否则从当前位置移除，然后添加到合适位置
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}

/**
 * 从链表中删除定时器
 * @param timer 要删除的定时器
 */
void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if ((timer == head) && (timer == tail))
    {
        // 链表中只有一个定时器
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    if (timer == head)
    {
        // 定时器是头节点
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail)
    {
        // 定时器是尾节点
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    // 定时器在链表中间
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

/**
 * 处理超时定时器
 * 遍历链表，处理所有过期的定时器
 */
void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }

    time_t cur = time(NULL); // 获取当前时间
    util_timer *tmp = head;
    while (tmp)
    {
        if (cur < tmp->expire)
        {
            // 找到第一个未过期的定时器，后面的也不会过期
            break;
        }
        // 处理过期定时器
        tmp->cb_func(tmp->user_data);
        // 从链表中移除并删除
        head = tmp->next;
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

/**
 * 内部方法：将定时器添加到指定头节点之后
 * @param timer 要添加的定时器
 * @param lst_head 链表头节点
 */
void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    while (tmp)
    {
        if (timer->expire < tmp->expire)
        {
            // 找到合适位置，插入到 prev 和 tmp 之间
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    if (!tmp)
    {
        // 到达链表尾部，作为新的尾节点
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

/**
 * 初始化工具类
 * @param timeslot 定时器时间间隔
 */
void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

/**
 * 对文件描述符设置非阻塞
 * @param fd 文件描述符
 * @return 原有的文件描述符状态
 */
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);      // 获取原有的文件状态标志
    int new_option = old_option | O_NONBLOCK; // 添加非阻塞标志
    fcntl(fd, F_SETFL, new_option);           // 设置新的文件状态标志
    return old_option;                        // 返回原有状态，便于恢复
}

/**
 * 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
 * @param epollfd epoll文件描述符
 * @param fd 要添加的文件描述符
 * @param one_shot 是否开启EPOLLONESHOT
 * @param TRIGMode 触发模式（0:LT, 1:ET）
 */
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP; // ET模式
    else
        event.events = EPOLLIN | EPOLLRDHUP; // LT模式

    if (one_shot)
        event.events |= EPOLLONESHOT;              // 开启EPOLLONESHOT
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event); // 添加到epoll
    setnonblocking(fd);                            // 设置非阻塞
}

/**
 * 信号处理函数
 * @param sig 信号编号
 */
void Utils::sig_handler(int sig)
{
    // 为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0); // 通过管道发送信号到主循环
    errno = save_errno;
}

/**
 * 设置信号函数
 * @param sig 信号编号
 * @param handler 信号处理函数
 * @param restart 是否自动重启被信号中断的系统调用
 */
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;           // 自动重启被中断的系统调用
    sigfillset(&sa.sa_mask);                 // 填充信号集
    assert(sigaction(sig, &sa, NULL) != -1); // 设置信号处理函数
}

/**
 * 定时处理任务，重新定时以不断触发SIGALRM信号
 */
void Utils::timer_handler()
{
    m_timer_lst.tick(); // 处理超时定时器
    alarm(m_TIMESLOT);  // 重新设置闹钟
}

/**
 * 向客户端发送错误信息
 * @param connfd 连接文件描述符
 * @param info 错误信息
 */
void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0); // 发送错误信息
    close(connfd);                       // 关闭连接
}

// 静态成员初始化
int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

// 前向声明
class Utils;

/**
 * 定时器回调函数
 * 处理超时的客户端连接
 * @param user_data 客户端数据
 */
void cb_func(client_data *user_data)
{
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0); // 从epoll中删除
    assert(user_data);
    close(user_data->sockfd);  // 关闭套接字
    http_conn::m_user_count--; // 减少用户计数
}