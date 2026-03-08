#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"

/**
 * 前向声明 util_timer 类
 */
class util_timer;

/**
 * 客户端数据结构
 * 用于存储客户端连接信息和对应的定时器
 */
struct client_data
{
    sockaddr_in address; // 客户端地址
    int sockfd;          // 客户端套接字文件描述符
    util_timer *timer;   // 指向该客户端对应的定时器
};

/**
 * 定时器类
 * 用于管理连接的超时时间
 */
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {} // 构造函数，初始化前后指针为NULL

public:
    time_t expire;                  // 定时器过期时间
    void (*cb_func)(client_data *); // 回调函数，用于处理超时连接
    client_data *user_data;         // 指向对应的客户端数据
    util_timer *prev;               // 前向指针，用于链表操作
    util_timer *next;               // 后向指针，用于链表操作
};

/**
 * 排序定时器链表类
 * 按照定时器过期时间排序，用于高效管理多个定时器
 */
class sort_timer_lst
{
public:
    sort_timer_lst();  // 构造函数
    ~sort_timer_lst(); // 析构函数

    /**
     * 添加定时器到链表
     * @param timer 要添加的定时器
     */
    void add_timer(util_timer *timer);

    /**
     * 调整定时器在链表中的位置
     * @param timer 要调整的定时器
     */
    void adjust_timer(util_timer *timer);

    /**
     * 从链表中删除定时器
     * @param timer 要删除的定时器
     */
    void del_timer(util_timer *timer);

    /**
     * 处理超时定时器
     */
    void tick();

private:
    /**
     * 内部方法：将定时器添加到指定头节点之后
     * @param timer 要添加的定时器
     * @param lst_head 链表头节点
     */
    void add_timer(util_timer *timer, util_timer *lst_head);

    util_timer *head; // 链表头节点
    util_timer *tail; // 链表尾节点
};

/**
 * 工具类
 * 提供信号处理、定时器管理等功能
 */
class Utils
{
public:
    Utils() {}  // 构造函数
    ~Utils() {} // 析构函数

    /**
     * 初始化工具类
     * @param timeslot 定时器时间间隔
     */
    void init(int timeslot);

    /**
     * 对文件描述符设置非阻塞
     * @param fd 文件描述符
     * @return 设置后的文件描述符
     */
    int setnonblocking(int fd);

    /**
     * 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
     * @param epollfd epoll文件描述符
     * @param fd 要添加的文件描述符
     * @param one_shot 是否开启EPOLLONESHOT
     * @param TRIGMode 触发模式（0:LT, 1:ET）
     */
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    /**
     * 信号处理函数
     * @param sig 信号编号
     */
    static void sig_handler(int sig);

    /**
     * 设置信号函数
     * @param sig 信号编号
     * @param handler 信号处理函数
     * @param restart 是否自动重启被信号中断的系统调用
     */
    void addsig(int sig, void(handler)(int), bool restart = true);

    /**
     * 定时处理任务，重新定时以不断触发SIGALRM信号
     */
    void timer_handler();

    /**
     * 向客户端发送错误信息
     * @param connfd 连接文件描述符
     * @param info 错误信息
     */
    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;       // 管道文件描述符，用于信号处理
    sort_timer_lst m_timer_lst; // 定时器链表
    static int u_epollfd;       // epoll文件描述符
    int m_TIMESLOT;             // 定时器时间间隔
};

/**
 * 定时器回调函数
 * 处理超时的客户端连接
 * @param user_data 客户端数据
 */
void cb_func(client_data *user_data);

#endif