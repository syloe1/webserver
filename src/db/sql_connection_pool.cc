#include "db/sql_connection_pool.h"
#include <mysql/mysql.h>
#include <queue>
#include <stdexcept>
#include <stdio.h>
#include <string>

using namespace std;

connection_pool::connection_pool() {
  m_CurConn = 0;
  m_FreeConn = 0;
  m_MaxConn = 0;
  m_close_log = 0;
}

connection_pool *connection_pool::GetInstance() {
  static connection_pool connPool;
  return &connPool;
}
// 创建单个 MySQL 连接（工具函数）
MYSQL *connection_pool::CreateMysqlConn() {
  MYSQL *con = mysql_init(nullptr);
  if (!con) {
    LOG_ERROR("mysql_init failed!");
    return nullptr;
  }

  // 端口是 int，不是 string！！！这里必须修正
  int port = stoi(m_port);

  con = mysql_real_connect(con, m_url.c_str(), m_user.c_str(), m_passwd.c_str(),
                           m_dbname.c_str(), port, nullptr, 0);
  if (!con) {
    LOG_ERROR("mysql_connect failed: %s", mysql_error(con));
    mysql_close(con);
    return nullptr;
  }
  return con;
}

// 校验连接是否有效，失效则重建
MYSQL *connection_pool::CheckAndReBuildConn(MYSQL *old_conn) {
  if (!old_conn)
    return nullptr;

  if (mysql_ping(old_conn) == 0) {
    return old_conn;
  }
  LOG_WARN("mysql connection lost, reconnecting... errno={}, msg={}",
           mysql_errno(old_conn), mysql_error(old_conn));
  mysql_close(old_conn);
  return CreateMysqlConn();
}
// 初始化连接池
void connection_pool::init(string url, string User, string PassWord,
                           string dbname, int port, int maxconn,
                           int close_log) {
  // 1. 把配置参数保存到成员变量
  m_url = url;
  m_user = User;
  m_passwd = PassWord;
  m_dbname = dbname;
  m_port = to_string(port); // int端口转字符串，刚好给CreateMysqlConn里面stoi用
  m_close_log = close_log;

  // 2. 循环创建 maxconn 个mysql连接
  for (int i = 0; i < maxconn; i++) {
    MYSQL *con = CreateMysqlConn();
    if (!con) {
      LOG_ERROR("create mysql connection failed");
      throw runtime_error("init connection pool failed");
    }
    connPool.push(con); // 创建成功，放进空闲连接队列
    m_FreeConn++;       // 空闲连接计数+1
  }

  reserve.reinit(m_FreeConn); // 信号量初始化，值=空闲连接数量
  m_MaxConn = m_FreeConn;     // 最大连接数 = 实际成功创建的连接数
}

// 阻塞获取连接
MYSQL *connection_pool::GetConnection() {
  reserve.wait();

  locker_guard guard(lock);
  if (connPool.empty())
    return nullptr;

  MYSQL *con = connPool.front();
  connPool.pop();

  m_FreeConn--;
  m_CurConn++;

  return CheckAndReBuildConn(con);
}

// 超时获取连接
MYSQL *connection_pool::GetConnection(int ms_timeout) {
  if (!reserve.timewait(ms_timeout)) {
    LOG_ERROR("get connection timeout");
    return nullptr;
  }

  locker_guard guard(lock);
  if (connPool.empty())
    return nullptr;

  MYSQL *con = connPool.front();
  connPool.pop();

  m_FreeConn--;
  m_CurConn++;

  return CheckAndReBuildConn(con);
}

// 归还连接
bool connection_pool::ReleaseConnection(MYSQL *conn) {
  if (!conn)
    return false;

  locker_guard guard(lock);
  connPool.push(conn);

  m_FreeConn++;
  m_CurConn--;

  reserve.post();
  return true;
}

// 销毁连接池
void connection_pool::DestroyPool() {
  // 不等待归还：直接关闭池中现有的连接
  // worker 线程持有的连接由 connectionRAII 析构自己归还/关闭
  locker_guard guard(lock);
  while (!connPool.empty()) {
    MYSQL *con = connPool.front();
    connPool.pop();
    mysql_close(con);
  }
  m_CurConn = 0;
  m_FreeConn = 0;
}

// 获取空闲连接数
int connection_pool::GetFreeConn() {
  locker_guard guard(lock);
  return m_FreeConn;
}

connection_pool::~connection_pool() { DestroyPool(); }

// ------------------- RAII -------------------
connectionRAII::connectionRAII(MYSQL **conn, connection_pool *connPool) {
  // 1. 从连接池获取一条连接，赋值给外部传入的 MYSQL* 变量
  *conn = connPool->GetConnection();
  // 2. 保存拿到的连接句柄，析构时用来归还
  connRAII = *conn;
  // 3. 保存连接池指针，析构时调用 ReleaseConnection
  pollRAII = connPool;
}

connectionRAII::~connectionRAII() {
  // 判空：如果获取连接失败，不用归还
  if (connRAII)
    pollRAII->ReleaseConnection(connRAII);
}
