#include "db/user_cache.h"
#include "core/locker.h"
#include "core/log.h"
#include "db/sql_connection_pool.h"
#include <mysql/mysql.h>
#include <string>

// ===================== 单例实现 =====================
UserCache *UserCache::getInstance() {
  static UserCache instance;
  return &instance;
}

UserCache::UserCache() : lock_(new locker()) {}

// ===================== load_all_users =====================
// 预加载数据库中全部用户到内存缓存
void UserCache::load_all_users(connection_pool *connPool) {
  // 先从连接池中取一个连接
  MYSQL *mysql = NULL;
  connectionRAII mysqlcon(&mysql, connPool);

  // 在user表中检索username，passwd数据
  if (mysql_query(mysql, "SELECT username,passwd FROM user")) {
    LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
  }

  // 从表中检索完整的结果集
  MYSQL_RES *result = mysql_store_result(mysql);

  // 从结果集中获取下一行，将对应的用户名和密码，存入map中
  lock_->lock();
  while (MYSQL_ROW row = mysql_fetch_row(result)) {
    std::string temp1(row[0]);
    std::string temp2(row[1]);
    users_[temp1] = temp2;
  }
  lock_->unlock();
}

// ===================== user_exists =====================
bool UserCache::user_exists(const std::string &name) {
  lock_->lock();
  bool exists = (users_.find(name) != users_.end());
  lock_->unlock();
  return exists;
}

// ===================== validate_user =====================
bool UserCache::validate_user(const std::string &name,
                              const std::string &password) {
  lock_->lock();
  bool valid = false;
  auto it = users_.find(name);
  if (it != users_.end() && it->second == password)
    valid = true;
  lock_->unlock();
  return valid;
}

// ===================== insert_user =====================
void UserCache::insert_user(const std::string &name,
                            const std::string &password) {
  lock_->lock();
  users_.insert(std::pair<std::string, std::string>(name, password));
  lock_->unlock();
}
