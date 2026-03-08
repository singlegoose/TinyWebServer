#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
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
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

/**
 * HTTP连接处理类
 * 负责处理HTTP请求的解析和响应
 */
class http_conn
{
public:
    // 常量定义
    static const int FILENAME_LEN = 200;       // 文件名长度
    static const int READ_BUFFER_SIZE = 2048;  // 读缓冲区大小
    static const int WRITE_BUFFER_SIZE = 1024; // 写缓冲区大小

    // HTTP请求方法枚举
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };

    // HTTP请求解析状态枚举
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0, // 解析请求行
        CHECK_STATE_HEADER,          // 解析请求头
        CHECK_STATE_CONTENT          // 解析请求体
    };

    // HTTP响应状态码枚举
    enum HTTP_CODE
    {
        NO_REQUEST,        // 未完成请求
        GET_REQUEST,       // 完整请求
        BAD_REQUEST,       // 错误请求
        NO_RESOURCE,       // 资源不存在
        FORBIDDEN_REQUEST, // 禁止访问
        FILE_REQUEST,      // 文件请求
        INTERNAL_ERROR,    // 内部错误
        CLOSED_CONNECTION  // 连接关闭
    };

    // 行解析状态枚举
    enum LINE_STATUS
    {
        LINE_OK = 0, // 行解析成功
        LINE_BAD,    // 行解析错误
        LINE_OPEN    // 行未结束
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
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
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);

    /**
     * 关闭连接
     * @param real_close 是否真正关闭连接
     */
    void close_conn(bool real_close = true);

    /**
     * 处理HTTP请求
     */
    void process();

    /**
     * 读取客户端数据
     * @return 是否读取成功
     */
    bool read_once();

    /**
     * 发送HTTP响应
     * @return 是否发送成功
     */
    bool write();

    /**
     * 获取客户端地址
     * @return 客户端地址指针
     */
    sockaddr_in *get_address()
    {
        return &m_address;
    }

    /**
     * 初始化数据库查询结果
     * @param connPool 数据库连接池
     */
    void initmysql_result(connection_pool *connPool);

    int timer_flag; // 定时器标志
    int improv;     // 改进标志

private:
    /**
     * 初始化连接状态
     */
    void init();

    /**
     * 处理HTTP请求的读取和解析
     * @return HTTP请求状态
     */
    HTTP_CODE process_read();

    /**
     * 处理HTTP响应的构建
     * @param ret HTTP请求状态
     * @return 是否构建成功
     */
    bool process_write(HTTP_CODE ret);

    /**
     * 解析HTTP请求行
     * @param text 请求行文本
     * @return HTTP请求状态
     */
    HTTP_CODE parse_request_line(char *text);

    /**
     * 解析HTTP请求头
     * @param text 头部文本
     * @return HTTP请求状态
     */
    HTTP_CODE parse_headers(char *text);

    /**
     * 解析HTTP请求体
     * @param text 内容文本
     * @return HTTP请求状态
     */
    HTTP_CODE parse_content(char *text);

    /**
     * 处理HTTP请求
     * @return HTTP请求状态
     */
    HTTP_CODE do_request();

    /**
     * 获取当前行
     * @return 当前行的指针
     */
    char *get_line() { return m_read_buf + m_start_line; };

    /**
     * 解析行
     * @return 行解析状态
     */
    LINE_STATUS parse_line();

    /**
     * 解除内存映射
     */
    void unmap();

    /**
     * 向响应缓冲区添加内容
     * @param format 格式化字符串
     * @param ... 可变参数
     * @return 是否添加成功
     */
    bool add_response(const char *format, ...);

    /**
     * 添加响应内容
     * @param content 内容
     * @return 是否添加成功
     */
    bool add_content(const char *content);

    /**
     * 添加HTTP状态行
     * @param status 状态码
     * @param title 状态描述
     * @return 是否添加成功
     */
    bool add_status_line(int status, const char *title);

    /**
     * 添加HTTP头部
     * @param content_length 内容长度
     * @return 是否添加成功
     */
    bool add_headers(int content_length);

    /**
     * 添加Content-Type头部
     * @return 是否添加成功
     */
    bool add_content_type();

    /**
     * 添加Content-Length头部
     * @param content_length 内容长度
     * @return 是否添加成功
     */
    bool add_content_length(int content_length);

    /**
     * 添加Connection头部
     * @return 是否添加成功
     */
    bool add_linger();

    /**
     * 添加空行
     * @return 是否添加成功
     */
    bool add_blank_line();

public:
    static int m_epollfd;    // 静态epoll文件描述符
    static int m_user_count; // 静态用户计数
    MYSQL *mysql;            // MySQL连接
    int m_state;             // 状态：读为0, 写为1

private:
    int m_sockfd;                        // 套接字文件描述符
    sockaddr_in m_address;               // 客户端地址
    char m_read_buf[READ_BUFFER_SIZE];   // 读缓冲区
    long m_read_idx;                     // 读缓冲区当前位置
    long m_checked_idx;                  // 读缓冲区检查位置
    int m_start_line;                    // 当前行的起始位置
    char m_write_buf[WRITE_BUFFER_SIZE]; // 写缓冲区
    int m_write_idx;                     // 写缓冲区当前位置
    CHECK_STATE m_check_state;           // 请求解析状态
    METHOD m_method;                     // HTTP请求方法
    char m_real_file[FILENAME_LEN];      // 真实文件路径
    char *m_url;                         // 请求URL
    char *m_version;                     // HTTP版本
    char *m_host;                        // Host头部
    long m_content_length;               // 内容长度
    bool m_linger;                       // 是否保持连接
    char *m_file_address;                // 文件内存映射地址
    struct stat m_file_stat;             // 文件状态
    struct iovec m_iv[2];                // 分散/聚集I/O
    int m_iv_count;                      // 分散/聚集I/O计数
    int cgi;                             // 是否启用POST
    char *m_string;                      // 存储请求头数据
    int bytes_to_send;                   // 待发送字节数
    int bytes_have_send;                 // 已发送字节数
    char *doc_root;                      // 文档根目录

    map<string, string> m_users; // 用户数据
    int m_TRIGMode;              // 触发模式
    int m_close_log;             // 日志开关

    char sql_user[100];   // 数据库用户名
    char sql_passwd[100]; // 数据库密码
    char sql_name[100];   // 数据库名称
};

#endif