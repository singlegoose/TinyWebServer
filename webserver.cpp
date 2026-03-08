#include "webserver.h"

/**
 * WebServer 构造函数
 * 初始化服务器的基本资源
 */
WebServer::WebServer()
{
    // 分配最大文件描述符数量的 http_conn 对象
    users = new http_conn[MAX_FD];

    // 确定服务器根目录路径
    char server_path[200];
    getcwd(server_path, 200);                                        // 获取当前工作目录
    char root[6] = "/root";                                          // 静态资源目录
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1); // 分配内存
    strcpy(m_root, server_path);                                     // 复制当前工作目录
    strcat(m_root, root);                                            // 拼接根目录路径

    // 分配定时器相关内存
    users_timer = new client_data[MAX_FD];
}

/**
 * WebServer 析构函数
 * 释放服务器资源
 */
WebServer::~WebServer()
{
    close(m_epollfd);     // 关闭 epoll 文件描述符
    close(m_listenfd);    // 关闭监听套接字
    close(m_pipefd[1]);   // 关闭管道写端
    close(m_pipefd[0]);   // 关闭管道读端
    delete[] users;       // 释放 http_conn 对象数组
    delete[] users_timer; // 释放定时器数据数组
    delete m_pool;        // 释放线程池
}

/**
 * 初始化服务器配置
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
void WebServer::init(int port, string user, string passWord, string databaseName, int log_write,
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

/**
 * 设置触发模式
 * 根据 m_TRIGMode 配置监听套接字和连接套接字的触发方式
 */
void WebServer::trig_mode()
{
    // LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0; // 监听套接字使用水平触发
        m_CONNTrigmode = 0;   // 连接套接字使用水平触发
    }
    // LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0; // 监听套接字使用水平触发
        m_CONNTrigmode = 1;   // 连接套接字使用边缘触发
    }
    // ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1; // 监听套接字使用边缘触发
        m_CONNTrigmode = 0;   // 连接套接字使用水平触发
    }
    // ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1; // 监听套接字使用边缘触发
        m_CONNTrigmode = 1;   // 连接套接字使用边缘触发
    }
}

/**
 * 初始化日志系统
 */
void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        // 初始化日志
        if (1 == m_log_write)
            // 异步日志模式
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            // 同步日志模式
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

/**
 * 初始化数据库连接池
 */
void WebServer::sql_pool()
{
    // 初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    // 初始化数据库读取表
    users->initmysql_result(m_connPool);
}

/**
 * 初始化线程池
 */
void WebServer::thread_pool()
{
    // 线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

/**
 * 设置事件监听
 * 包括创建套接字、绑定地址、开始监听、初始化 epoll 等
 */
void WebServer::eventListen()
{
    // 网络编程基础步骤
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    // 优雅关闭连接
    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    utils.init(TIMESLOT);

    // epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);

    utils.addsig(SIGPIPE, SIG_IGN);
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    alarm(TIMESLOT);

    // 工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

/**
 * 为新连接创建定时器
 * @param connfd 连接文件描述符
 * @param client_address 客户端地址
 */
void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    // 初始化client_data数据
    // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer);
}

/**
 * 调整定时器
 * 若有数据传输，则将定时器往后延迟3个单位
 * @param timer 要调整的定时器
 */
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

/**
 * 处理定时器超时
 * @param timer 超时的定时器
 * @param sockfd 对应的套接字
 */
void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

/**
 * 处理新的客户端连接
 * @return 处理是否成功
 *         - LT 模式：返回 true 表示处理成功，false 表示处理失败
 *         - ET 模式：始终返回 false，表示需要继续处理（循环已处理所有连接）
 */
bool WebServer::dealclientdata()
{
    struct sockaddr_in client_address;                    // 客户端地址结构体
    socklen_t client_addrlength = sizeof(client_address); // 地址长度

    // 水平触发（LT）模式
    if (0 == m_LISTENTrigmode)
    {
        // 接受新连接
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);

        // 检查 accept 是否失败
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }

        // 检查连接数是否超过最大值
        if (http_conn::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy"); // 显示服务器繁忙错误
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }

        // 为新连接创建定时器
        timer(connfd, client_address);
    }

    // 边缘触发（ET）模式
    else
    {
        // 循环接受所有待处理的连接（ET 模式下需要一次性处理完所有连接）
        while (1)
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);

            // 检查 accept 是否失败（ET 模式下，没有更多连接时会返回 EAGAIN）
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break; // 跳出循环，等待下一次事件触发
            }

            // 检查连接数是否超过最大值
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy"); // 显示服务器繁忙错误
                LOG_ERROR("%s", "Internal server busy");
                break; // 跳出循环，等待下一次事件触发
            }

            // 为新连接创建定时器
            timer(connfd, client_address);
        }

        return false; // ET 模式下返回 false，表示已处理完所有连接
    }

    return true; // LT 模式下返回 true，表示处理成功
}

/**
 * 处理信号
 * 从管道中读取信号，并根据信号类型设置相应的标志
 * @param timeout 超时标志，用于通知主循环处理定时器超时
 * @param stop_server 停止服务器标志，用于通知主循环停止服务器
 * @return 处理是否成功
 */
bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;        // 读取结果
    int sig;            // 信号值
    char signals[1024]; // 存储读取到的信号

    // 从管道中读取信号，管道的写端在信号处理函数中写入信号
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);

    // 检查读取是否失败
    if (ret == -1)
    {
        return false;
    }
    // 检查管道是否关闭
    else if (ret == 0)
    {
        return false;
    }
    // 处理读取到的信号
    else
    {
        // 遍历所有读取到的信号
        for (int i = 0; i < ret; ++i)
        {
            // 根据信号类型设置相应的标志
            switch (signals[i])
            {
            case SIGALRM: // 定时器信号
            {
                timeout = true; // 设置超时标志，通知主循环处理定时器
                break;
            }
            case SIGTERM: // 终止信号
            {
                stop_server = true; // 设置停止服务器标志，通知主循环停止服务器
                break;
            }
            }
        }
    }
    return true; // 处理成功
}

/**
 * 处理读事件
 * 根据反应堆模型（Reactor/Proactor）采取不同的处理策略
 * @param sockfd 套接字文件描述符
 */
void WebServer::dealwithread(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer; // 获取该连接对应的定时器

    // Reactor 模型：工作线程负责读写操作
    if (1 == m_actormodel)
    {
        // 如果有定时器，调整定时器（延长超时时间）
        if (timer)
        {
            adjust_timer(timer);
        }

        // 若监测到读事件，将该事件放入请求队列，状态为 0（读）
        m_pool->append(users + sockfd, 0);

        // 等待任务处理完成
        while (true)
        {
            // 检查任务是否处理完成（improv 标志为 1）
            if (1 == users[sockfd].improv)
            {
                // 检查是否需要关闭连接（timer_flag 标志为 1）
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);    // 处理定时器，关闭连接
                    users[sockfd].timer_flag = 0; // 重置定时器标志
                }
                users[sockfd].improv = 0; // 重置处理完成标志
                break;                    // 跳出循环，结束处理
            }
        }
    }
    else
    {
        // Proactor 模型：主线程负责读写操作，工作线程只负责业务逻辑
        if (users[sockfd].read_once()) // 读取数据
        {
            // 记录日志，显示客户端 IP
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            // 若监测到读事件，将该事件放入请求队列（Proactor 模式）
            m_pool->append_p(users + sockfd);

            // 如果有定时器，调整定时器（延长超时时间）
            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            // 读取失败，处理定时器，关闭连接
            deal_timer(timer, sockfd);
        }
    }
}
/**
 * 处理写事件
 * 根据反应堆模型（Reactor/Proactor）采取不同的处理策略
 * @param sockfd 套接字文件描述符
 */
void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer; // 获取该连接对应的定时器

    // Reactor 模型：工作线程负责写操作
    if (1 == m_actormodel)
    {
        // 如果有定时器，调整定时器（延长超时时间）
        if (timer)
        {
            adjust_timer(timer);
        }

        // 将写事件放入请求队列，状态为 1（写）
        m_pool->append(users + sockfd, 1);

        // 等待任务处理完成
        while (true)
        {
            // 检查任务是否处理完成（improv 标志为 1）
            if (1 == users[sockfd].improv)
            {
                // 检查是否需要关闭连接（timer_flag 标志为 1）
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);    // 处理定时器，关闭连接
                    users[sockfd].timer_flag = 0; // 重置定时器标志
                }
                users[sockfd].improv = 0; // 重置处理完成标志
                break;                    // 跳出循环，结束处理
            }
        }
    }
    else
    {
        // Proactor 模型：主线程负责写操作，工作线程只负责业务逻辑
        if (users[sockfd].write()) // 写入数据
        {
            // 记录日志，显示客户端 IP
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            // 如果有定时器，调整定时器（延长超时时间）
            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            // 写入失败，处理定时器，关闭连接
            deal_timer(timer, sockfd);
        }
    }
}

/**
 * 服务器主事件循环
 * 负责处理所有 epoll 事件，包括新连接、读写事件、信号和定时器超时
 */
void WebServer::eventLoop()
{
    bool timeout = false;     // 定时器超时标志
    bool stop_server = false; // 服务器停止标志

    // 主循环，直到服务器停止
    while (!stop_server)
    {
        // 调用 epoll_wait 等待事件，-1 表示无限等待
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        // 检查 epoll_wait 是否失败（排除被信号中断的情况）
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        // 遍历所有触发的事件
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd; // 获取事件对应的文件描述符

            // 处理新到的客户连接
            if (sockfd == m_listenfd)
            {
                bool flag = dealclientdata(); // 处理新连接
                if (false == flag)            // 如果处理失败，继续处理下一个事件
                    continue;
            }
            // 处理连接错误（连接被对端关闭或发生错误）
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // 服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            // 处理信号（通过管道传递）
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                bool flag = dealwithsignal(timeout, stop_server); // 处理信号
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            // 处理客户连接上接收到的数据（读事件）
            else if (events[i].events & EPOLLIN)
            {
                dealwithread(sockfd);
            }
            // 处理客户连接上的写事件
            else if (events[i].events & EPOLLOUT)
            {
                dealwithwrite(sockfd);
            }
        }
        // 处理定时器超时
        if (timeout)
        {
            utils.timer_handler(); // 处理所有超时的定时器

            LOG_INFO("%s", "timer tick");

            timeout = false; // 重置超时标志
        }
    }
}