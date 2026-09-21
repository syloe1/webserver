#ifndef USER_CACHE_H
#define USER_CACHE_H

#include <map>
#include <string>

class connection_pool;
class locker;

// ============================================================
// 全局数据库用户缓存（单例）
// 封装原 http_conn.cpp 中全局 users map + m_lock
// 提供线程安全的用户查询/插入接口
// ============================================================
class UserCache {
public:
  // 获取单例实例
  static UserCache *getInstance();

  // 从数据库 user 表预加载全部用户名/密码到内存缓存
  void load_all_users(connection_pool *connPool);

  // 检查用户名是否已存在（线程安全）
  bool user_exists(const std::string &name);

  // 校验用户名密码是否匹配（线程安全）
  bool validate_user(const std::string &name, const std::string &password);

  // 插入新用户到缓存（线程安全）
  void insert_user(const std::string &name, const std::string &password);

private:
  UserCache(); // 构造私有， 只能通过getInstance() 拿到唯一全局对象
  ~UserCache() = default;

  // 禁止拷贝、赋值
  UserCache(const UserCache &) = delete;
  UserCache &operator=(const UserCache &) = delete;
  std::map<std::string, std::string> users_;
  locker *lock_;
};

#endif
