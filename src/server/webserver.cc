#include "server/webserver.h"
#include "core/log.h"
#include <cstdio>
#include <cstring>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

// ===================== 构造函数 =====================
WebServer::WebServer()
    : m_port(0), m_root(nullptr), m_log_write(0), m_close_log(0),
      m_actormodel(0), m_epollfd(-1), m_MAX_FD(DEFAULT_MAX_FD),
      m_MAX_EVENT_NUMBER(DEFAULT_MAX_EVENT_NUMBER),
      m_TIMESLOT(DEFAULT_TIMESLOT), users(nullptr), users_timer(nullptr),
      m_connPool(nullptr), m_sql_num(0), m_pool(nullptr), m_thread_num(0),
      events(nullptr), m_listenfd(-1), m_OPT_LINGER(0), m_TRIGMode(0),
      m_LISTENTrigmode(0), m_CONNTrigmode(0) {
  // 根据动态最大fd分配数组
  users = new http_conn[m_MAX_FD];
  users_timer = new client_data[m_MAX_FD];

  // 初始化网站根目录
  char server_path[200];
  getcwd(server_path, sizeof(server_path));
  const char sub_root[] = "/static";
  size_t total_len = strlen(server_path) + strlen(sub_root) + 1;
  m_root = (char *)malloc(total_len);
  strcpy(m_root, server_path);
  strcat(m_root, sub_root);

  // 动态分配epoll事件数组
  events = new epoll_event[m_MAX_EVENT_NUMBER];

  // 管道初始化为-1
  m_pipefd[0] = m_pipefd[1] = -1;
}

// ===================== 析构函数 完整释放所有资源 =====================
WebServer::~WebServer() {
  // 关闭内核fd
  if (m_epollfd >= 0)
    close(m_epollfd);
  if (m_listenfd >= 0)
    close(m_listenfd);
  if (m_pipefd[1] >= 0)
    close(m_pipefd[1]);
  if (m_pipefd[0] >= 0)
    close(m_pipefd[0]);

  // 释放堆数组
  delete[] users;
  delete[] users_timer;
  delete[] events;

  // 销毁线程池
  delete m_pool;

  // 释放根目录字符串
  if (m_root)
    free(m_root);
}

// ===================== init 补齐动态配置参数 =====================
void WebServer::init(int port, std::string user, std::string passWord,
                     std::string databaseName, int log_write, int opt_linger,
                     int trigmode, int sql_num, int thread_num, int close_log,
                     int actor_model, int timeslot, int max_fd, int max_event) {
  m_port = port;
  m_user = user;
  m_passWord = passWord;
  m_databaseName = databaseName;
  m_sql_num = sql_num;
  m_thread_num = thread_num;
  m_log_write = log_write;
  m_OPT_LINGER = opt_linger;
  m_TRIGMode = trigmode;
  m_close_log = close_log;
  m_actormodel = actor_model;

  // 动态覆盖上限配置
  m_TIMESLOT = timeslot;
  m_MAX_FD = max_fd;
  m_MAX_EVENT_NUMBER = max_event;
}

// ===================== set_root 实现 =====================
void WebServer::set_root(const char *root) {
  if (!root)
    return;
  if (m_root)
    free(m_root);
  size_t len = strlen(root) + 1;
  m_root = (char *)malloc(len);
  strcpy(m_root, root);
}

// ===================== 全部只读Getter实现 =====================
int WebServer::get_port() const { return m_port; }
const char *WebServer::get_root() const { return m_root; }
int WebServer::get_log_write() const { return m_log_write; }
int WebServer::get_close_log() const { return m_close_log; }
int WebServer::get_actor_model() const { return m_actormodel; }
int WebServer::get_epollfd() const { return m_epollfd; }
int WebServer::get_listenfd() const { return m_listenfd; }
int WebServer::get_opt_linger() const { return m_OPT_LINGER; }
int WebServer::get_trig_mode() const { return m_TRIGMode; }
int WebServer::get_listen_trig_mode() const { return m_LISTENTrigmode; }
int WebServer::get_conn_trig_mode() const { return m_CONNTrigmode; }
int WebServer::get_sql_num() const { return m_sql_num; }
int WebServer::get_thread_num() const { return m_thread_num; }
int WebServer::get_time_slot() const { return m_TIMESLOT; }
int WebServer::get_max_fd() const { return m_MAX_FD; }
int WebServer::get_max_event() const { return m_MAX_EVENT_NUMBER; }

std::string WebServer::get_db_user() const { return m_user; }
std::string WebServer::get_db_passwd() const { return m_passWord; }
std::string WebServer::get_db_name() const { return m_databaseName; }

// ===================== trig_mode =====================
void WebServer::trig_mode() {
  if (0 == m_TRIGMode) {
    m_LISTENTrigmode = 0;
    m_CONNTrigmode = 0;
  } else if (1 == m_TRIGMode) {
    m_LISTENTrigmode = 0;
    m_CONNTrigmode = 1;
  } else if (2 == m_TRIGMode) {
    m_LISTENTrigmode = 1;
    m_CONNTrigmode = 0;
  } else if (3 == m_TRIGMode) {
    m_LISTENTrigmode = 1;
    m_CONNTrigmode = 1;
  }
}

// ===================== log_write =====================
void WebServer::log_write() {
  if (0 == m_close_log) {
    if (1 == m_log_write)
      Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
    else
      Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
  }
}

// ===================== sql_pool =====================
void WebServer::sql_pool() {
  m_connPool = connection_pool::GetInstance();
  m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306,
                   m_sql_num, m_close_log);

  m_conn_lock.lock();
  users->initmysql_result(m_connPool);
  m_conn_lock.unlock();
}

// ===================== thread_pool =====================
void WebServer::thread_pool() {
  m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

// ===================== eventListen =====================
void WebServer::eventListen() {
  m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
  assert(m_listenfd >= 0);

  if (0 == m_OPT_LINGER) {
    struct linger tmp = {0, 1};
    setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
  } else if (1 == m_OPT_LINGER) {
    struct linger tmp = {1, 1};
    setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
  }

  int ret = 0;
  struct sockaddr_in address;
  bzero(&address, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(m_port);

  int flag = 1;
  setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
  ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
  assert(ret >= 0);
  ret = listen(m_listenfd, 5);
  assert(ret >= 0);

  utils.init(m_TIMESLOT);

  m_epollfd = epoll_create(5);
  assert(m_epollfd != -1);

  utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
  http_conn::m_epollfd = m_epollfd;

  ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
  assert(ret != -1);
  utils.setnonblocking(m_pipefd[1]);
  utils.addfd(m_epollfd, m_pipefd[0], false, 0);

  utils.addsig(SIGPIPE, SIG_IGN);
  utils.addsig(SIGALRM, utils.sig_handler, false);
  utils.addsig(SIGTERM, utils.sig_handler, false);

  alarm(m_TIMESLOT);

  Utils::u_pipefd[0] = m_pipefd[0];
  Utils::u_pipefd[1] = m_pipefd[1];
  Utils::u_epollfd = m_epollfd;
}

// ===================== timer 新建连接绑定定时器（加锁） =====================
void WebServer::timer(int connfd, struct sockaddr_in client_address) {
  m_conn_lock.lock();
  users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode,
                     m_close_log, m_user, m_passWord, m_databaseName);
  m_conn_lock.unlock();

  users_timer[connfd].address = client_address;
  users_timer[connfd].sockfd = connfd;
  util_timer *timer = new util_timer;
  timer->user_data = &users_timer[connfd];
  timer->cb_func = cb_func;
  time_t cur = time(nullptr);
  timer->expire = cur + 3 * m_TIMESLOT;
  users_timer[connfd].timer = timer;

  m_timer_lock.lock();
  utils.m_timer_lst.add_timer(timer);
  m_timer_lock.unlock();
}

// ===================== adjust_timer 刷新超时 =====================
void WebServer::adjust_timer(util_timer *timer) {
  if (!timer)
    return;
  time_t cur = time(nullptr);
  timer->expire = cur + 3 * m_TIMESLOT;

  m_timer_lock.lock();
  utils.m_timer_lst.adjust_timer(timer);
  m_timer_lock.unlock();

  LOG_INFO("%s", "adjust timer once");
}

// ===================== deal_timer 删除定时器 =====================
void WebServer::deal_timer(util_timer *timer, int sockfd) {
  if (!timer)
    return;
  timer->cb_func(&users_timer[sockfd]);

  m_timer_lock.lock();
  utils.m_timer_lst.del_timer(timer);
  m_timer_lock.unlock();

  LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

// ===================== dealclientdata 处理新连接 =====================
bool WebServer::dealclientdata() {
  struct sockaddr_in client_address;
  socklen_t client_addrlength = sizeof(client_address);
  if (0 == m_LISTENTrigmode) {
    int connfd = accept(m_listenfd, (struct sockaddr *)&client_address,
                        &client_addrlength);
    if (connfd < 0) {
      LOG_ERROR("%s:errno is:%d", "accept error", errno);
      return false;
    }
    m_conn_lock.lock();
    int user_cnt = http_conn::m_user_count;
    m_conn_lock.unlock();
    if (user_cnt >= m_MAX_FD) {
      utils.show_error(connfd, "Internal server busy");
      LOG_ERROR("%s", "Internal server busy");
      return false;
    }
    timer(connfd, client_address);
  } else {
    while (1) {
      int connfd = accept(m_listenfd, (struct sockaddr *)&client_address,
                          &client_addrlength);
      if (connfd < 0) {
        LOG_ERROR("%s:errno is:%d", "accept error", errno);
        break;
      }
      m_conn_lock.lock();
      int user_cnt = http_conn::m_user_count;
      m_conn_lock.unlock();
      if (user_cnt >= m_MAX_FD) {
        utils.show_error(connfd, "Internal server busy");
        LOG_ERROR("%s", "Internal server busy");
        break;
      }
      timer(connfd, client_address);
    }
    return false;
  }
  return true;
}

// ===================== dealwithsignal 管道信号处理 =====================
bool WebServer::dealwithsignal(bool &timeout, bool &stop_server) {
  int ret = 0;
  int sig;
  char signals[1024];
  ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
  if (ret == -1 || ret == 0) {
    return false;
  }
  for (int i = 0; i < ret; ++i) {
    switch (signals[i]) {
    case SIGALRM:
      timeout = true;
      break;
    case SIGTERM:
      stop_server = true;
      break;
    }
  }
  return true;
}

// ===================== dealwithread 读事件 =====================
void WebServer::dealwithread(int sockfd) {
  util_timer *timer = users_timer[sockfd].timer;
  if (1 == m_actormodel) {
    if (timer) {
      adjust_timer(timer);
    }
    m_pool->append(users + sockfd, 0);
    while (true) {
      m_conn_lock.lock();
      int improv = users[sockfd].improv;
      int t_flag = users[sockfd].timer_flag;
      m_conn_lock.unlock();
      if (1 == improv) {
        if (1 == t_flag) {
          deal_timer(timer, sockfd);
          m_conn_lock.lock();
          users[sockfd].timer_flag = 0;
          users[sockfd].improv = 0;
          m_conn_lock.unlock();
        } else {
          m_conn_lock.lock();
          users[sockfd].improv = 0;
          m_conn_lock.unlock();
        }
        break;
      }
    }
  } else {
    m_conn_lock.lock();
    bool read_ok = users[sockfd].read_once();
    m_conn_lock.unlock();
    if (read_ok) {
      LOG_INFO("deal with the client(%s)",
               inet_ntoa(users[sockfd].get_address()->sin_addr));
      m_pool->append_p(users + sockfd);
      if (timer) {
        adjust_timer(timer);
      }
    } else {
      deal_timer(timer, sockfd);
    }
  }
}

// ===================== dealwithwrite 写事件 =====================
void WebServer::dealwithwrite(int sockfd) {
  util_timer *timer = users_timer[sockfd].timer;
  if (1 == m_actormodel) {
    if (timer) {
      adjust_timer(timer);
    }
    m_pool->append(users + sockfd, 1);
    while (true) {
      m_conn_lock.lock();
      int improv = users[sockfd].improv;
      int t_flag = users[sockfd].timer_flag;
      m_conn_lock.unlock();
      if (1 == improv) {
        if (1 == t_flag) {
          deal_timer(timer, sockfd);
          m_conn_lock.lock();
          users[sockfd].timer_flag = 0;
          users[sockfd].improv = 0;
          m_conn_lock.unlock();
        } else {
          m_conn_lock.lock();
          users[sockfd].improv = 0;
          m_conn_lock.unlock();
        }
        break;
      }
    }
  } else {
    m_conn_lock.lock();
    bool write_ok = users[sockfd].write();
    m_conn_lock.unlock();
    if (write_ok) {
      LOG_INFO("send data to the client(%s)",
               inet_ntoa(users[sockfd].get_address()->sin_addr));
      if (timer) {
        adjust_timer(timer);
      }
    } else {
      deal_timer(timer, sockfd);
    }
  }
}

// ===================== eventLoop 主循环 =====================
void WebServer::eventLoop() {
  bool timeout = false;
  bool stop_server = false;
  while (!stop_server) {
    int number = epoll_wait(m_epollfd, events, m_MAX_EVENT_NUMBER, -1);
    if (number < 0 && errno != EINTR) {
      LOG_ERROR("%s", "epoll failure");
      break;
    }
    for (int i = 0; i < number; i++) {
      int sockfd = events[i].data.fd;
      if (sockfd == m_listenfd) {
        bool flag = dealclientdata();
        if (false == flag)
          continue;
      } else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        util_timer *timer = users_timer[sockfd].timer;
        deal_timer(timer, sockfd);
      } else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)) {
        bool flag = dealwithsignal(timeout, stop_server);
        if (false == flag)
          LOG_ERROR("%s", "dealclientdata failure");
      } else if (events[i].events & EPOLLIN) {
        dealwithread(sockfd);
      } else if (events[i].events & EPOLLOUT) {
        dealwithwrite(sockfd);
      }
    }
    if (timeout) {
      utils.timer_handler();
      LOG_INFO("%s", "timer tick");
      timeout = false;
    }
  }
}
