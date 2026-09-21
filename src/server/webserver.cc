#include "server/webserver.h"

#include <cstdio>
#include <cstring>
#include <csignal>
#include <unistd.h>

#include "core/log.h"
#include "db/user_cache.h"
// Scheduler 里有 unordered_map<int, unique_ptr<http_conn>>，实例化 Scheduler
// 时需要 http_conn 是完整类型（否则 unique_ptr 的 static_assert 会炸）
#include "net/http_conn.h"

namespace {
// 信号处理函数没有 this，用它把当前实例接出去。只在 eventLoop 里赋值一次。
WebServer *g_signal_target = nullptr;
} // namespace

// ===================== 构造函数 =====================
WebServer::WebServer() {
  char server_path[200];
  if (!getcwd(server_path, sizeof(server_path)))
    server_path[0] = '\0';
  const char sub_root[] = "/static";
  const size_t total_len = strlen(server_path) + strlen(sub_root) + 1;
  m_root = static_cast<char *>(malloc(total_len));
  strcpy(m_root, server_path);
  strcat(m_root, sub_root);
}

WebServer::~WebServer() {
  // 先停机再 join，否则载体线程可能还在引用即将析构的成员
  for (coro::Scheduler *s : m_sched_raw)
    s->stop();
  for (std::thread &t : m_carriers) {
    if (t.joinable())
      t.join();
  }
  m_carriers.clear();
  m_scheds.clear();
  m_sched_raw.clear();
  m_blocking.reset();
  if (m_root)
    free(m_root);
}

void WebServer::init(int port, std::string user, std::string passWord,
                     std::string databaseName, int log_write, int opt_linger,
                     int trigmode, int sql_num, int thread_num, int close_log,
                     int actor_model, int timeslot, int max_fd, int max_event) {
  m_port = port;
  m_user = std::move(user);
  m_passWord = std::move(passWord);
  m_databaseName = std::move(databaseName);
  m_sql_num = sql_num;
  m_thread_num = (thread_num > 0) ? thread_num : 1;
  m_log_write = log_write;
  m_OPT_LINGER = opt_linger;
  m_close_log = close_log;
  m_actormodel = actor_model;
  m_TIMESLOT = timeslot;
  m_MAX_FD = max_fd;
  m_MAX_EVENT_NUMBER = max_event;
  // 注意：这里不能打日志。init() 跑在 log_write() 之前，日志模块还没
  // 初始化（m_fp 为空，write_log 会直接丢弃），告警会静默消失。
  // 两个 epoll 时代的开关记下来，等 eventListen() 里日志就绪后再报。
  m_trigmode = trigmode;
}

void WebServer::set_root(const char *root) {
  if (!root)
    return;
  if (m_root)
    free(m_root);
  const size_t len = strlen(root) + 1;
  m_root = static_cast<char *>(malloc(len));
  strcpy(m_root, root);
}

// ===================== 只读 Getter =====================
int WebServer::get_port() const { return m_port; }
const char *WebServer::get_root() const { return m_root; }
int WebServer::get_log_write() const { return m_log_write; }
int WebServer::get_close_log() const { return m_close_log; }
int WebServer::get_actor_model() const { return m_actormodel; }
int WebServer::get_opt_linger() const { return m_OPT_LINGER; }
int WebServer::get_sql_num() const { return m_sql_num; }
int WebServer::get_thread_num() const { return m_thread_num; }
int WebServer::get_time_slot() const { return m_TIMESLOT; }
int WebServer::get_max_fd() const { return m_MAX_FD; }
int WebServer::get_max_event() const { return m_MAX_EVENT_NUMBER; }
std::string WebServer::get_db_user() const { return m_user; }
std::string WebServer::get_db_passwd() const { return m_passWord; }
std::string WebServer::get_db_name() const { return m_databaseName; }

// ===================== 日志 / 数据库 =====================
void WebServer::log_write() {
  if (0 == m_close_log) {
    if (1 == m_log_write)
      Log::get_instance()->init("./ServerLog", m_close_log, 2000, 500, 800);
    else
      Log::get_instance()->init("./ServerLog", m_close_log, 2000, 500, 0);
  } else {
    Log::get_instance()->m_close_log = 1;
  }
}

void WebServer::sql_pool() {
  m_connPool = connection_pool::GetInstance();
  m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306,
                   m_sql_num, m_close_log);
  // 用户表一次性读进内存缓存，之后登录走缓存、注册才碰数据库
  UserCache::getInstance()->load_all_users(m_connPool);
  m_opt.conn_pool = m_connPool;
}

// ===================== 阻塞卸载池 =====================
void WebServer::thread_pool() {
  // 载体线程上只跑非阻塞代码，MySQL 这类阻塞调用全部卸载到这里。
  // 池子不需要很大：只有注册请求会真正排队。
  m_blocking = std::make_unique<coro::BlockingPool>(
      (m_sql_num > 0) ? static_cast<unsigned>(m_sql_num) : 4u);
}

void WebServer::trig_mode() {
  // io_uring 提交即异步，不存在水平/边缘触发的选择
}

// ===================== eventListen =====================
void WebServer::eventListen() {
  // 日志此刻已初始化，失效开关在这里告警才真正落盘
  if (m_trigmode != 0)
    LOG_WARN("-m %d ignored: io_uring has no LT/ET trigger mode", m_trigmode);
  if (m_actormodel != 0)
    LOG_WARN("-a %d ignored: coroutine version always runs Proactor-style "
             "co_await I/O",
             m_actormodel);

  m_opt.port = m_port;
  m_opt.opt_linger = (m_OPT_LINGER != 0);
  m_opt.close_log = m_close_log;
  m_opt.doc_root = m_root ? m_root : "";
  m_opt.db_user = m_user;
  m_opt.db_passwd = m_passWord;
  m_opt.db_name = m_databaseName;
  m_opt.tick_ms = m_TIMESLOT * 1000;
  m_opt.idle_timeout_ms = 3 * m_TIMESLOT * 1000;
  // 每个调度器一份连接配额，总量仍是 -f 指定的上限
  m_opt.max_conns = (m_MAX_FD / m_thread_num > 0) ? (m_MAX_FD / m_thread_num) : 1;

  m_scheds.reserve(static_cast<size_t>(m_thread_num));
  m_sched_raw.reserve(static_cast<size_t>(m_thread_num));

  for (int i = 0; i < m_thread_num; ++i) {
    auto s = std::make_unique<coro::Scheduler>();
    if (!s->init(m_opt, i, m_blocking.get())) {
      LOG_ERROR("failed to init scheduler %d, aborting", i);
      m_scheds.clear();
      m_sched_raw.clear();
      return;
    }
    m_sched_raw.push_back(s.get());
    m_scheds.push_back(std::move(s));
  }

  LOG_INFO("coroutine server ready: port=%d carriers=%d max_conns=%d "
           "doc_root=%s",
           m_port, m_thread_num, m_MAX_FD, m_opt.doc_root.c_str());
}

// ===================== 信号 =====================
// 只做 write：Scheduler::stop() 是"原子置位 + 写 eventfd"，
// 两者都是异步信号安全的，handler 里不会碰锁或分配器。
void WebServer::on_signal(int sig) {
  (void)sig;
  if (!g_signal_target)
    return;
  for (coro::Scheduler *s : g_signal_target->m_sched_raw)
    s->stop();
}

// ===================== eventLoop =====================
void WebServer::eventLoop() {
  if (m_scheds.empty()) {
    LOG_ERROR("no scheduler available, server not started");
    return;
  }

  g_signal_target = this;

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = &WebServer::on_signal;
  sigemptyset(&sa.sa_mask);
  // 不设 SA_RESTART：载体线程等在 io_uring 上，与 EINTR 无关；
  // 主线程只是 join，也不需要重启语义
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  // 写已关闭的对端会立刻拿到 EPIPE 而不是 SIGPIPE，避免进程被信号打死
  signal(SIGPIPE, SIG_IGN);

  for (size_t i = 0; i < m_scheds.size(); ++i) {
    coro::Scheduler *s = m_scheds[i].get();
    m_carriers.emplace_back([s] { s->run(); });
  }

  for (std::thread &t : m_carriers) {
    if (t.joinable())
      t.join();
  }
  LOG_INFO("server stopped");
}
