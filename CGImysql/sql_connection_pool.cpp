#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

/**
 * connection_pool 构造函数
 * 初始化连接池的基本状态
 */
connection_pool::connection_pool()
{
	m_CurConn = 0;	// 当前已使用的连接数初始化为0
	m_FreeConn = 0; // 当前空闲的连接数初始化为0
}

/**
 * 获取连接池实例（单例模式）
 * @return 连接池实例指针
 */
connection_pool *connection_pool::GetInstance()
{
	static connection_pool connPool; // 静态局部变量，保证唯一实例
	return &connPool;
}

/**
 * 初始化连接池
 * @param url 主机地址
 * @param User 数据库用户名
 * @param PassWord 数据库密码
 * @param DBName 数据库名称
 * @param Port 数据库端口号
 * @param MaxConn 最大连接数
 * @param close_log 日志开关
 */
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
	m_url = url;
	m_Port = Port;
	m_User = User;
	m_PassWord = PassWord;
	m_DatabaseName = DBName;
	m_close_log = close_log;

	// 创建指定数量的数据库连接
	for (int i = 0; i < MaxConn; i++)
	{
		MYSQL *con = NULL;
		con = mysql_init(con); // 初始化MYSQL对象

		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}
		// 连接到数据库
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);

		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}
		connList.push_back(con); // 将连接添加到连接池
		++m_FreeConn;			 // 空闲连接数加1
	}

	reserve = sem(m_FreeConn); // 初始化信号量，值为空闲连接数

	m_MaxConn = m_FreeConn; // 设置最大连接数
}

/**
 * 从连接池获取可用连接
 * @return 可用的数据库连接
 */
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL;

	if (0 == connList.size())
		return NULL;

	reserve.wait(); // 信号量减1，等待可用连接

	lock.lock(); // 加锁，保证线程安全

	con = connList.front(); // 获取链表第一个连接
	connList.pop_front();	// 从链表中移除

	--m_FreeConn; // 空闲连接数减1
	++m_CurConn;  // 当前使用连接数加1

	lock.unlock(); // 解锁
	return con;
}

/**
 * 释放数据库连接
 * @param con 要释放的数据库连接
 * @return 释放是否成功
 */
bool connection_pool::ReleaseConnection(MYSQL *con)
{
	if (NULL == con)
		return false;

	lock.lock(); // 加锁，保证线程安全

	connList.push_back(con); // 将连接放回连接池
	++m_FreeConn;			 // 空闲连接数加1
	--m_CurConn;			 // 当前使用连接数减1

	lock.unlock(); // 解锁

	reserve.post(); // 信号量加1，通知等待的线程
	return true;
}

/**
 * 销毁数据库连接池
 * 关闭所有连接并释放资源
 */
void connection_pool::DestroyPool()
{
	lock.lock(); // 加锁，保证线程安全
	if (connList.size() > 0)
	{
		list<MYSQL *>::iterator it;
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			mysql_close(con); // 关闭数据库连接
		}
		m_CurConn = 0;
		m_FreeConn = 0;
		connList.clear(); // 清空连接列表
	}

	lock.unlock(); // 解锁
}

/**
 * 获取当前空闲的连接数
 * @return 空闲连接数
 */
int connection_pool::GetFreeConn()
{
	return this->m_FreeConn;
}

/**
 * connection_pool 析构函数
 * 销毁连接池
 */
connection_pool::~connection_pool()
{
	DestroyPool();
}

/**
 * connectionRAII 构造函数
 * 获取数据库连接并管理
 * @param SQL 输出参数，用于存储获取的连接
 * @param connPool 连接池指针
 */
connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool)
{
	*SQL = connPool->GetConnection(); // 从连接池获取连接

	conRAII = *SQL;		 // 保存连接
	poolRAII = connPool; // 保存连接池指针
}

/**
 * connectionRAII 析构函数
 * 自动释放数据库连接
 */
connectionRAII::~connectionRAII()
{
	poolRAII->ReleaseConnection(conRAII); // 将连接归还到连接池
}