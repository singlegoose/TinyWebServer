#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"

/**
 * 常量定义
 */
const int MAX_FD = 65536;           // 最大文件描述符数量
const int MAX_EVENT_NUMBER = 10000; // 最大事件数
const int TIMESLOT = 5;             // 最小超时单位（秒）

/**
 * WebServer 类
 * 服务器的核心控制类，负责管理服务器的整个生命周期
 */
class WebServer
{
public:
    /**
     * 构造函数
     */
    WebServer();

    /**
     * 析构函数
     */
    ~WebServer();

    /**
     * 初始化服务器
     * @param port 端口号
     * @param user 数据库用户名
     * @param passWord 数据库密码
     * @param databaseName 数据库名称
     * @param log_write 日志写入方式（0:同步, 1:异步）
     * @param opt_linger 优雅关闭连接选项
     * @param trigmode 触发模式
     * @param sql_num 数据库连接池大小
     * @param thread_num 线程池大小
     * @param close_log 是否关闭日志
     * @param actor_model 反应堆模型（0:Proactor, 1:Reactor）
     */
    void init(int port, string user, string passWord, string databaseName,
              int log_write, int opt_linger, int trigmode, int sql_num,
              int thread_num, int close_log, int actor_model);

    /**
     * 初始化线程池
     */
    void thread_pool();

    /**
     * 初始化数据库连接池
     */
    void sql_pool();

    /**
     * 初始化日志系统
     */
    void log_write();

    /**
     * 设置触发模式
     */
    void trig_mode();

    /**
     * 设置事件监听
     */
    void eventListen();

    /**
     * 服务器主事件循环
     */
    void eventLoop();

    /**
     * 为新连接创建定时器
     * @param connfd 连接文件描述符
     * @param client_address 客户端地址
     */
    void timer(int connfd, struct sockaddr_in client_address);

    /**
     * 调整定时器
     * @param timer 要调整的定时器
     */
    void adjust_timer(util_timer *timer);

    /**
     * 处理定时器超时
     * @param timer 超时的定时器
     * @param sockfd 对应的套接字
     */
    void deal_timer(util_timer *timer, int sockfd);

    /**
     * 处理新客户端连接
     * @return 处理是否成功
     */
    bool dealclientdata();

    /**
     * 处理信号
     * @param timeout 超时标志
     * @param stop_server 停止服务器标志
     * @return 处理是否成功
     */
    bool dealwithsignal(bool &timeout, bool &stop_server);

    /**
     * 处理读事件
     * @param sockfd 套接字文件描述符
     */
    void dealwithread(int sockfd);

    /**
     * 处理写事件
     * @param sockfd 套接字文件描述符
     */
    void dealwithwrite(int sockfd);

public:
    // 基础配置
    int m_port;       // 服务器端口号
    char *m_root;     // 网站根目录
    int m_log_write;  // 日志写入方式
    int m_close_log;  // 是否关闭日志
    int m_actormodel; // 反应堆模型

    int m_pipefd[2];  // 用于信号处理的管道
    int m_epollfd;    // epoll文件描述符
    http_conn *users; // HTTP连接数组

    // 数据库相关
    connection_pool *m_connPool; // 数据库连接池
    string m_user;               // 数据库用户名
    string m_passWord;           // 数据库密码
    string m_databaseName;       // 数据库名称
    int m_sql_num;               // 数据库连接池大小

    // 线程池相关
    threadpool<http_conn> *m_pool; // 线程池
    int m_thread_num;              // 线程池大小

    // epoll_event相关
    epoll_event events[MAX_EVENT_NUMBER]; // 事件数组

    int m_listenfd;       // 监听套接字
    int m_OPT_LINGER;     // 优雅关闭连接选项
    int m_TRIGMode;       // 触发模式
    int m_LISTENTrigmode; // 监听套接字触发模式
    int m_CONNTrigmode;   // 连接套接字触发模式

    // 定时器相关
    client_data *users_timer; // 客户端数据数组
    Utils utils;              // 工具类
};
#endif