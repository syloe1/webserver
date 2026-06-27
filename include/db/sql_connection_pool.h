#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include "core/locker.h"
#include "core/log.h"
#include <error.h>
#include <iostream>
#include <mysql/mysql.h>
#include <queue>
#include <stdio.h>
#include <string.h>
#include <string>

using namespace std;
class connection_pool {
public:
  MYSQL *GetConnection();
  // 超时获取连接，ms_timeout毫秒无空闲返回nullptr，防止线程卡死
  MYSQL *GetConnection(int ms_timeout);
  bool ReleaseConnection(MYSQL *conn);
  int GetFreeConn();
  void DestroyPool();

  // 单例模式
  static connection_pool *GetInstance();

  void init(string url, string User, string PassWord, string dbname, int port,
            int maxconn, int close_log);

private:
  // 创建单个mysql连接，内部工具函数
  MYSQL *CreateMysqlConn();
  // 校验连接是否有效，失效则重建
  MYSQL *CheckAndReBuildConn(MYSQL *old_conn);

private:
  string m_url;
  string m_port;
  string m_user;
  string m_passwd;
  string m_dbname;
  int m_close_log;

private:
  connection_pool();
  ~connection_pool();

  int m_MaxConn;
  int m_CurConn;
  int m_FreeConn;
  locker lock;
  queue<MYSQL *> connPool;
  sem reserve;

  // 禁止拷贝， 赋值
  connection_pool(const connection_pool &) = delete;
  connection_pool &operator=(const connection_pool &) = delete;
};
class connectionRAII {
public:
  connectionRAII(MYSQL **conn, connection_pool *connPool);
  ~connectionRAII();

private:
  MYSQL *connRAII;
  connection_pool *pollRAII;

  connectionRAII(const connectionRAII &) = delete;
  connectionRAII &operator=(const connectionRAII &) = delete;
};
#endif
