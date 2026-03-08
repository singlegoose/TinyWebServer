#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

// 定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

// 全局变量，用于线程安全的用户数据访问
locker m_lock;
map<string, string> users;

/**
 * 初始化数据库查询结果
 * @param connPool 数据库连接池
 */
void http_conn::initmysql_result(connection_pool *connPool)
{
    // 先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    // 在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    // 从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    // 返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    // 返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    // 从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

/**
 * 对文件描述符设置非阻塞
 * @param fd 文件描述符
 * @return 原有的文件状态标志
 */
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

/**
 * 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
 * @param epollfd epoll文件描述符
 * @param fd 要添加的文件描述符
 * @param one_shot 是否开启EPOLLONESHOT
 * @param TRIGMode 触发模式（0:LT, 1:ET）
 */
/**
 * 将文件描述符添加到 epoll 事件表中
 * @param epollfd epoll 文件描述符
 * @param fd 要添加的文件描述符
 * @param one_shot 是否开启 EPOLLONESHOT 模式
 * @param TRIGMode 触发模式（0: 水平触发 LT, 1: 边缘触发 ET）
 */
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;  // 定义 epoll 事件结构体
    event.data.fd = fd; // 设置事件关联的文件描述符

    // 根据触发模式设置事件类型
    if (1 == TRIGMode)
        // 边缘触发模式，添加 EPOLLET 标志
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        // 水平触发模式
        event.events = EPOLLIN | EPOLLRDHUP;

    // 如果需要，开启 EPOLLONESHOT 模式
    // EPOLLONESHOT 确保每个事件只被一个线程处理，避免并发问题
    if (one_shot)
        event.events |= EPOLLONESHOT;

    // 将文件描述符添加到 epoll 事件表
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);

    // 将文件描述符设置为非阻塞模式
    // 非阻塞模式可以提高服务器的响应速度和并发处理能力
    setnonblocking(fd);
}

/**
 * 从内核时间表删除描述符
 * @param epollfd epoll文件描述符
 * @param fd 要删除的文件描述符
 */
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

/**
 * 修改 epoll 事件表中文件描述符的事件设置
 * @param epollfd epoll 文件描述符
 * @param fd 要修改的文件描述符
 * @param ev 要设置的事件类型（如 EPOLLIN 或 EPOLLOUT）
 * @param TRIGMode 触发模式（0: 水平触发 LT, 1: 边缘触发 ET）
 */
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;  // 定义 epoll 事件结构体
    event.data.fd = fd; // 设置事件关联的文件描述符

    // 根据触发模式设置事件类型
    if (1 == TRIGMode)
        // 边缘触发模式，添加 EPOLLET 标志
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        // 水平触发模式
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    // 修改 epoll 事件表中的事件设置
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 静态成员初始化
// 统计当前服务器的活跃连接数
int http_conn::m_user_count = 0;
// 存储 epoll 实例的文件描述符，初始值为 -1 表示未初始化
int http_conn::m_epollfd = -1;

/**
 * 关闭连接
 * @param real_close 是否真正关闭连接
 */
/**
 * 关闭 HTTP 连接，释放相关资源
 * @param real_close 布尔值，决定是否真正关闭连接
 */
void http_conn::close_conn(bool real_close)
{
    // 检查是否需要真正关闭连接且连接有效
    if (real_close && (m_sockfd != -1))
    {
        // 打印关闭的文件描述符，便于调试
        printf("close %d\n", m_sockfd);

        // 从 epoll 事件表中移除该文件描述符
        removefd(m_epollfd, m_sockfd);

        // 将套接字文件描述符设为 -1，表示连接已关闭
        m_sockfd = -1;

        // 更新当前服务器的连接数
        m_user_count--;
    }
}

/**
 * 初始化连接
 * @param sockfd 套接字文件描述符
 * @param addr 客户端地址
 * @param root 网站根目录
 * @param TRIGMode 触发模式
 * @param close_log 日志开关
 * @param user 数据库用户名
 * @param passwd 数据库密码
 * @param sqlname 数据库名称
 */
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    // 当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

/**
 * 初始化新接受的连接
 * 重置连接状态和缓冲区
 */
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

/**
 * 从状态机，用于分析出一行内容
 * @return 行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
 */
/**
 * 解析 HTTP 请求中的一行数据
 * 从状态机，用于分析出一行内容
 * @return 行的读取状态：LINE_OK（行解析成功）、LINE_BAD（行格式错误）、LINE_OPEN（行未结束）
 */
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp; // 临时存储当前字符

    // 遍历读取缓冲区，从已检查位置到当前读取位置
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx]; // 获取当前字符

        // 处理回车符 '\r'
        if (temp == '\r')
        {
            // 如果 '\r' 是缓冲区的最后一个字符，说明行未结束
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            // 如果 '\r' 后面跟着 '\n'，说明行结束
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                // 将 '\r' 和 '\n' 替换为字符串结束符 '\0'
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK; // 行解析成功
            }
            // 否则，行格式错误
            return LINE_BAD;
        }
        // 处理换行符 '\n'
        else if (temp == '\n')
        {
            // 如果 '\n' 前面是 '\r'，说明行结束（标准 HTTP 行结束格式）
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                // 将 '\r' 替换为 '\0'，'\n' 也替换为 '\0'
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK; // 行解析成功
            }
            // 否则，行格式错误
            return LINE_BAD;
        }
    }
    // 遍历完所有字符仍未找到行结束符，说明行未结束
    return LINE_OPEN;
}
/**
 * 循环读取客户数据，直到无数据可读或对方关闭连接
 * @return 是否读取成功
 */
bool http_conn::read_once()
{
    // 检查读缓冲区是否已满
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    // LT（水平触发）模式读取数据
    if (0 == m_TRIGMode)
    {
        // 调用 recv 读取数据，最多读取 READ_BUFFER_SIZE - m_read_idx 字节
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        // 检查读取结果
        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    // ET（边缘触发）模式读取数据
    else
    {
        // 循环读取，直到没有更多数据可读
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                // 如果错误码是 EAGAIN 或 EWOULDBLOCK，表示数据已读完
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                // 其他错误，返回 false
                return false;
            }
            else if (bytes_read == 0)
            {
                // 连接关闭，返回 false
                return false;
            }
            // 更新读缓冲区指针
            m_read_idx += bytes_read;
        }
        return true;
    }
}

/**
 * 解析http请求行，获得请求方法，目标url及http版本号
 * @param text 请求行文本
 * @return HTTP请求状态
 */
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // 查找请求行中的空格或制表符，分离请求方法和URL
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST; // URL 不存在，返回错误
    }
    *m_url++ = '\0'; // 将空格替换为字符串结束符，使 text 指向请求方法

    char *method = text; // 提取请求方法
    if (strcasecmp(method, "GET") == 0)
        m_method = GET; // 设置为 GET 方法
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST; // 设置为 POST 方法
        cgi = 1;         // POST 方法需要启用 CGI
    }
    else
        return BAD_REQUEST; // 不支持的方法，返回错误

    // 跳过 URL 前的空格和制表符
    m_url += strspn(m_url, " \t");

    // 查找 URL 和 HTTP 版本之间的空格或制表符
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST; // HTTP 版本不存在，返回错误

    *m_version++ = '\0'; // 将空格替换为字符串结束符，使 m_url 指向完整的 URL

    // 跳过 HTTP 版本前的空格和制表符
    m_version += strspn(m_version, " \t");

    // 验证 HTTP 版本是否为 HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;

    // 处理 URL 中的 http:// 前缀
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/'); // 找到路径部分
    }

    // 处理 URL 中的 https:// 前缀
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/'); // 找到路径部分
    }

    // 验证 URL 格式是否正确
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;

    // 当 URL 为 "/" 时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");

    // 更新状态为解析请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST; // 返回 NO_REQUEST 表示需要继续解析
}

/**
 * 解析http请求的一个头部信息
 * @param text 头部信息文本
 * @return HTTP请求状态
 */
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 检查是否到达请求头的末尾（空行）
    if (text[0] == '\0')
    {
        // 如果存在内容长度，说明请求有正文，需要继续解析
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT; // 更新状态为解析请求体
            return NO_REQUEST;                   // 返回 NO_REQUEST 表示需要继续解析
        }
        // 否则，请求解析完成
        return GET_REQUEST;
    }
    // 处理 Connection 头
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;                  // 跳过 "Connection:"
        text += strspn(text, " \t"); // 跳过空格和制表符
        // 如果是 keep-alive，设置连接为长连接
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    // 处理 Content-length 头
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;                    // 跳过 "Content-length:"
        text += strspn(text, " \t");   // 跳过空格和制表符
        m_content_length = atol(text); // 解析内容长度
    }
    // 处理 Host 头
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;                   // 跳过 "Host:"
        text += strspn(text, " \t"); // 跳过空格和制表符
        m_host = text;               // 存储主机名
    }
    // 处理其他未知头
    else
    {
        LOG_INFO("oop!unknow header: %s", text); // 记录日志
    }
    return NO_REQUEST; // 返回 NO_REQUEST 表示需要继续解析
}
/**
 * 判断http请求是否被完整读入
 * @param text 请求内容文本
 * @return HTTP请求状态
 */
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        // POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

/**
 * 处理HTTP请求的读取和解析
 * @return HTTP请求状态
 */
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK; // 行解析状态
    HTTP_CODE ret = NO_REQUEST;        // HTTP 请求解析状态
    char *text = 0;                    // 当前解析的行文本

    // 主循环：处理请求行、请求头和请求体
    // 条件1：当前状态是解析请求体且行解析成功
    // 条件2：解析一行成功
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) ||
           ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();            // 获取当前行
        m_start_line = m_checked_idx; // 更新起始行位置
        LOG_INFO("%s", text);         // 记录日志

        // 根据当前解析状态处理
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE: // 解析请求行
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST; // 请求行解析错误
            break;
        }
        case CHECK_STATE_HEADER: // 解析请求头
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST; // 请求头解析错误
            else if (ret == GET_REQUEST)
            {
                return do_request(); // 请求解析完成，处理请求
            }
            break;
        }
        case CHECK_STATE_CONTENT: // 解析请求体
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request(); // 请求解析完成，处理请求
            line_status = LINE_OPEN; // 请求体可能跨多行，设置为行未结束
            break;
        }
        default:
            return INTERNAL_ERROR; // 状态错误
        }
    }
    return NO_REQUEST; // 请求未完全解析，需要继续读取数据
}

/**
 * 处理 HTTP 请求
 * @return HTTP 请求状态码
 */
http_conn::HTTP_CODE http_conn::do_request()
{
    // 构建文件路径：将网站根目录复制到 m_real_file
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    // 找到 URL 中最后一个 '/' 的位置
    const char *p = strrchr(m_url, '/');

    // 处理 CGI 请求（登录/注册）
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        // 根据标志判断是登录检测还是注册检测
        // '2' 表示登录，'3' 表示注册
        char flag = m_url[1];

        // 构建真实文件路径
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        // 从 POST 请求正文中提取用户名和密码
        // 格式为：user=123&passwd=123
        char name[100], password[100];
        int i;
        // 提取用户名（从 "user=" 之后到 "&" 之前）
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        // 提取密码（从 "passwd=" 之后到字符串结束）
        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        // 处理注册请求
        if (*(p + 1) == '3')
        {
            // 构建 SQL 插入语句
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            // 检查用户名是否已存在
            if (users.find(name) == users.end())
            {
                // 加锁，保证线程安全
                m_lock.lock();
                // 执行 SQL 插入操作
                int res = mysql_query(mysql, sql_insert);
                // 更新内存中的用户数据
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                // 根据操作结果跳转页面
                if (!res)
                    strcpy(m_url, "/log.html"); // 注册成功，跳转到登录页
                else
                    strcpy(m_url, "/registerError.html"); // 注册失败
            }
            else
                strcpy(m_url, "/registerError.html"); // 用户名已存在
        }
        // 处理登录请求
        else if (*(p + 1) == '2')
        {
            // 检查用户名和密码是否匹配
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html"); // 登录成功，跳转到欢迎页
            else
                strcpy(m_url, "/logError.html"); // 登录失败
        }
    }

    // 处理其他特殊 URL 请求
    if (*(p + 1) == '0')
    {
        // 注册页面
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        // 登录页面
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        // 图片页面
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        // 视频页面
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        // 粉丝页面
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else
        // 其他 URL，直接附加到根目录后面
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    // 检查文件状态
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE; // 文件不存在

    // 检查文件是否有其他用户可读权限
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST; // 权限不足

    // 检查文件是否为目录
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST; // 是目录，请求错误

    // 打开文件并映射到内存
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST; // 文件请求成功
}
/**
 * 解除内存映射
 */
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

/**
 * 发送 HTTP 响应
 * @return 是否发送成功
 */
bool http_conn::write()
{
    int temp = 0; // 临时变量，存储每次 writev 的返回值

    // 检查是否有数据需要发送
    if (bytes_to_send == 0)
    {
        // 数据已发送完毕，将事件模式改为 EPOLLIN，等待下一次请求
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        // 初始化连接状态
        init();
        return true;
    }

    // 循环发送数据，直到全部发送完毕
    while (1)
    {
        // 使用 writev 进行分散/聚集 I/O，同时发送响应头和响应体
        // m_iv[0] 存储响应头，m_iv[1] 存储响应体（文件内容）
        temp = writev(m_sockfd, m_iv, m_iv_count);

        // 处理发送错误
        if (temp < 0)
        {
            // EAGAIN 表示暂时无可用缓冲区，需要等待
            if (errno == EAGAIN)
            {
                // 将事件模式改为 EPOLLOUT，等待可写事件
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            // 其他错误，释放内存映射并返回失败
            unmap();
            return false;
        }

        // 更新已发送和待发送的字节数
        bytes_have_send += temp;
        bytes_to_send -= temp;

        // 检查响应头是否发送完毕
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            // 响应头已发送完毕，重置响应头缓冲区
            m_iv[0].iov_len = 0;
            // 更新响应体的起始位置和长度
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            // 响应头未发送完毕，更新响应头缓冲区的起始位置和长度
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        // 检查所有数据是否发送完毕
        if (bytes_to_send <= 0)
        {
            // 释放内存映射
            unmap();
            // 将事件模式改为 EPOLLIN，等待下一次请求
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            // 根据连接模式决定是否保持连接
            if (m_linger)
            {
                // 保持连接，初始化连接状态
                init();
                return true;
            }
            else
            {
                // 关闭连接，返回 false
                return false;
            }
        }
    }
}

/**
 * 向响应缓冲区添加格式化内容
 * @param format 格式化字符串
 * @param ... 可变参数列表
 * @return 是否添加成功
 */
bool http_conn::add_response(const char *format, ...)
{
    // 检查写入缓冲区是否已满
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;

    // 初始化可变参数列表
    va_list arg_list;
    va_start(arg_list, format);

    // 使用 vsnprintf 格式化内容到写入缓冲区
    // 计算写入的长度，确保不超过缓冲区剩余空间
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);

    // 检查写入的长度是否超过缓冲区剩余空间
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        // 结束可变参数列表并返回失败
        va_end(arg_list);
        return false;
    }

    // 更新写入指针位置
    m_write_idx += len;
    // 结束可变参数列表
    va_end(arg_list);

    // 记录日志，显示响应内容
    LOG_INFO("request:%s", m_write_buf);

    // 返回成功
    return true;
}

/**
 * 添加HTTP状态行
 * @param status 状态码
 * @param title 状态描述
 * @return 是否添加成功
 */
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

/**
 * 添加HTTP头部
 * @param content_len 内容长度
 * @return 是否添加成功
 */
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}

/**
 * 添加Content-Length头部
 * @param content_len 内容长度
 * @return 是否添加成功
 */
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

/**
 * 添加Content-Type头部
 * @return 是否添加成功
 */
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

/**
 * 添加Connection头部
 * @return 是否添加成功
 */
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

/**
 * 添加空行
 * @return 是否添加成功
 */
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

/**
 * 添加响应内容
 * @param content 内容
 * @return 是否添加成功
 */
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

/**
 * 构建 HTTP 响应
 * 根据 HTTP 请求的处理结果，构建相应的 HTTP 响应
 * @param ret HTTP 请求的处理结果
 * @return 响应构建是否成功
 */
bool http_conn::process_write(HTTP_CODE ret)
{
    // 根据 HTTP 状态码构建相应的响应
    switch (ret)
    {
    case INTERNAL_ERROR: // 内部错误（500）
    {
        // 添加状态行：HTTP/1.1 500 Internal Error
        add_status_line(500, error_500_title);
        // 添加头部：Content-Length、Connection 等
        add_headers(strlen(error_500_form));
        // 添加错误信息内容
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST: // 错误请求（400）
    {
        // 添加状态行：HTTP/1.1 400 Bad Request
        add_status_line(404, error_404_title); // 注意：这里应该是 400，可能是代码错误
        // 添加头部
        add_headers(strlen(error_404_form));
        // 添加错误信息内容
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST: // 禁止访问（403）
    {
        // 添加状态行：HTTP/1.1 403 Forbidden
        add_status_line(403, error_403_title);
        // 添加头部
        add_headers(strlen(error_403_form));
        // 添加错误信息内容
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST: // 文件请求成功（200）
    {
        // 添加状态行：HTTP/1.1 200 OK
        add_status_line(200, ok_200_title);

        // 如果文件大小不为 0
        if (m_file_stat.st_size != 0)
        {
            // 添加头部，包含文件大小
            add_headers(m_file_stat.st_size);

            // 设置分散/聚集 I/O 的参数
            // m_iv[0] 存储响应头
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            // m_iv[1] 存储文件内容（内存映射地址）
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            // 设置 I/O 向量数量
            m_iv_count = 2;
            // 计算总发送字节数
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            // 如果文件大小为 0，发送空 HTML 页面
            const char *ok_string = "<html><body></body></html>";
            // 添加头部，包含内容长度
            add_headers(strlen(ok_string));
            // 添加空 HTML 内容
            if (!add_content(ok_string))
                return false;
        }
    }
    default: // 其他状态码，返回失败
        return false;
    }

    // 对于非文件请求（如错误响应），设置分散/聚集 I/O 的参数
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

/**
 * 处理 HTTP 连接的主函数
 * 完成请求的读取、解析和响应的构建
 */
void http_conn::process()
{
    // 读取并解析 HTTP 请求，返回 HTTP 状态码
    HTTP_CODE read_ret = process_read();

    // 如果请求未完全解析（需要更多数据）
    if (read_ret == NO_REQUEST)
    {
        // 将事件模式改为 EPOLLIN，等待更多数据
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }

    // 根据请求处理结果构建 HTTP 响应
    bool write_ret = process_write(read_ret);

    // 如果响应构建失败
    if (!write_ret)
    {
        // 关闭连接
        close_conn();
    }

    // 将事件模式改为 EPOLLOUT，等待可写事件，准备发送响应
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}