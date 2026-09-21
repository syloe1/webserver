#ifndef WEBSERVER_H
#define WEBSERVER_H

// ============================================================
// WebServer —— 协程版启动器（M:N 调度）
//
// 与 dev 分支（单 ring + 回调 + 业务线程池）的结构差别：
//
//   dev:  1 个 io_uring + 1 个 eventLoop 线程
//         + N 个 worker 线程从队列里抢 http_conn 干活
//
//   coro: N 个载体线程，每线程独占 1 个 io_uring + 1 个 listen socket
//         （SO_REUSEPORT 让内核按四元组分流，连接天然归属接受它的线程，
//          不再需要跨线程交接 connfd）
//         + 1 个阻塞卸载池，专收 MySQL 这类无法异步化的阻塞调用
//
// 所以 -t 的语义从"业务线程数"变成了"载体线程数"，
// -a / -m 两个 epoll 时代的开关保留但只告警不生效。
// ============================================================

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "coro/offload.h"
#include "coro/scheduler.h"
#include "db/sql_connection_pool.h"

// 全局默认常量，可通过 init 参数动态覆盖
const int DEFAULT_MAX_FD = 65536;
const int DEFAULT_TIMESLOT = 5;

class WebServer {
public:
  WebServer();
  ~WebServer();

  // 初始化，支持动态传入超时、最大 fd、事件上限，覆盖默认常量
  void init(int port, std::string user, std::string passWord,
            std::string databaseName, int log_write, int opt_linger,
            int trigmode, int sql_num, int thread_num, int close_log,
            int actor_model, int timeslot = DEFAULT_TIMESLOT,
            int max_fd = DEFAULT_MAX_FD, int max_event = 10000);

  void log_write();
  void sql_pool();
  // 建的是阻塞卸载池（MySQL 用），不是原业务线程池
  void thread_pool();
  // io_uring 没有 LT/ET 触发模式的概念，保留接口只为兼容启动流程
  void trig_mode();
  // 建 N 个 Scheduler（每个含独立 ring + eventfd + listen socket）
  void eventListen();
  // 起 N 个载体线程并 join 到停机
  void eventLoop();

  // ========== 只读 Getter 接口，外部仅能读取，不可修改 ==========
  int get_port() const;
  const char *get_root() const;
  int get_log_write() const;
  int get_close_log() const;
  int get_actor_model() const;
  int get_opt_linger() const;
  int get_sql_num() const;
  int get_thread_num() const;
  int get_time_slot() const;
  int get_max_fd() const;
  int get_max_event() const;

  std::string get_db_user() const;
  std::string get_db_passwd() const;
  std::string get_db_name() const;

  // 设置网站根目录（仅初始化阶段调用，运行时禁止修改）
  void set_root(const char *root);

  // 禁止拷贝构造、赋值，持有 io_uring/线程/连接池等不可复制资源
  WebServer(const WebServer &) = delete;
  WebServer &operator=(const WebServer &) = delete;

private:
  // 信号处理函数只做一件事：把所有调度器的 eventfd 写一遍
  static void on_signal(int sig);

  // ========== 全部成员私有，外部无法直接访问 ==========
  int m_port = 0;
  char *m_root = nullptr;
  int m_log_write = 0;
  int m_close_log = 0;
  int m_actormodel = 0;
  int m_OPT_LINGER = 0;
  // 只用于在日志就绪后提示"该开关已失效"
  int m_trigmode = 0;

  // 动态可配置上限，替代全局硬编码常量
  int m_MAX_FD = DEFAULT_MAX_FD;
  int m_MAX_EVENT_NUMBER = 10000;
  int m_TIMESLOT = DEFAULT_TIMESLOT;

  std::string m_user;
  std::string m_passWord;
  std::string m_databaseName;
  int m_sql_num = 0;

  // 业务/载体线程数
  int m_thread_num = 1;

  // 数据库连接池
  connection_pool *m_connPool = nullptr;

  // ★ 声明顺序即析构逆序：m_blocking 必须最后被销毁（调度器持有它的裸指针），
  //   所以它要声明在最前面。
  std::unique_ptr<coro::BlockingPool> m_blocking;
  std::vector<std::unique_ptr<coro::Scheduler>> m_scheds;
  std::vector<std::thread> m_carriers;

  // 信号处理函数里遍历用。启动后不再变动，避免在 handler 里碰 vector 的
  // 分配器/迭代器（那不是异步信号安全的）。
  std::vector<coro::Scheduler *> m_sched_raw;

  coro::Scheduler::Options m_opt;
};

#endif
