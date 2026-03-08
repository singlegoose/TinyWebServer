#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"
#include "../log/log.h"

using namespace std;

/**
 * 数据库连接池类
 * 管理数据库连接的创建、获取、释放和销毁
 */
class connection_pool
{
public:
	/**
	 * 获取数据库连接
	 * @return 返回一个可用的数据库连接
	 */
	MYSQL *GetConnection();

	/**
	 * 释放数据库连接
	 * @param conn 要释放的数据库连接
	 * @return 释放是否成功
	 */
	bool ReleaseConnection(MYSQL *conn);

	/**
	 * 获取当前空闲连接数
	 * @return 空闲连接数
	 */
	int GetFreeConn();

	/**
	 * 销毁所有连接
	 */
	void DestroyPool();

	/**
	 * 单例模式获取连接池实例
	 * @return 连接池实例指针
	 */
	static connection_pool *GetInstance();

	/**
	 * 初始化连接池
	 * @param url 主机地址
	 * @param User 数据库用户名
	 * @param PassWord 数据库密码
	 * @param DataBaseName 数据库名称
	 * @param Port 数据库端口号
	 * @param MaxConn 最大连接数
	 * @param close_log 日志开关
	 */
	void init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn, int close_log);

private:
	/**
	 * 构造函数（私有，单例模式）
	 */
	connection_pool();

	/**
	 * 析构函数（私有，单例模式）
	 */
	~connection_pool();

	int m_MaxConn;			// 最大连接数
	int m_CurConn;			// 当前已使用的连接数
	int m_FreeConn;			// 当前空闲的连接数
	locker lock;			// 互斥锁，用于线程同步
	list<MYSQL *> connList; // 连接池，存储数据库连接
	sem reserve;			// 信号量，用于控制连接的获取

public:
	string m_url;		   // 主机地址
	string m_Port;		   // 数据库端口号
	string m_User;		   // 登陆数据库用户名
	string m_PassWord;	   // 登陆数据库密码
	string m_DatabaseName; // 使用数据库名
	int m_close_log;	   // 日志开关
};

/**
 * 连接资源管理类（RAII模式）
 * 用于自动管理数据库连接的获取和释放
 */
class connectionRAII
{

public:
	/**
	 * 构造函数，获取数据库连接
	 * @param con 输出参数，用于存储获取的连接
	 * @param connPool 连接池指针
	 */
	connectionRAII(MYSQL **con, connection_pool *connPool);

	/**
	 * 析构函数，自动释放数据库连接
	 */
	~connectionRAII();

private:
	MYSQL *conRAII;			   // 获取的数据库连接
	connection_pool *poolRAII; // 连接池指针
};

#endif