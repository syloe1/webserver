#include "server/webserver.h"
#include "core/log.h"
#include <cstdio>
#include <cstring>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

static char g_accept_marker = 0;
static char g_wakeup_marker = 0;

// ===================== 构造函数 =====================
WebServer::WebServer()
    : m_port(0), m_root(nullptr), m_log_write(0), m_close_log(0),
      m_actormodel(0), m_MAX_FD(DEFAULT_MAX_FD),
      m_TIMESLOT(DEFAULT_TIMESLOT), users(nullptr), users_timer(nullptr),
      m_connPool(nullptr), m_sql_num(0), m_pool(nullptr), m_thread_num(0),
      m_listenfd(-1), m_OPT_LINGER(0) {
  users       = new http_conn[m_MAX_FD];
  users_timer = new client_data[m_MAX_FD];
  char server_path[200];
  if (!getcwd(server_path, sizeof(server_path))) server_path[0] = '\0';
  const char sub_root[] = "/static";
  size_t total_len = strlen(server_path) + strlen(sub_root) + 1;
  m_root = (char *)malloc(total_len);
  strcpy(m_root, server_path);
  strcat(m_root, sub_root);
  m_pipefd[0] = m_pipefd[1] = -1;
  memset(m_signal_buf, 0, sizeof(m_signal_buf));
}

WebServer::~WebServer() {
  m_uring.destroy();
  if (m_listenfd >= 0) close(m_listenfd);
  if (m_pipefd[1] >= 0) close(m_pipefd[1]);
  if (m_pipefd[0] >= 0) close(m_pipefd[0]);
  delete[] users;
  delete[] users_timer;
  delete m_pool;
  if (m_root) free(m_root);
}

void WebServer::init(int port, std::string user, std::string passWord,
                     std::string databaseName, int log_write, int opt_linger,
                     int trigmode, int sql_num, int thread_num, int close_log,
                     int actor_model, int timeslot, int max_fd, int max_event) {
  (void)trigmode; (void)max_event;
  m_port = port; m_user = user; m_passWord = passWord;
  m_databaseName = databaseName; m_sql_num = sql_num;
  m_thread_num = thread_num; m_log_write = log_write;
  m_OPT_LINGER = opt_linger; m_close_log = close_log;
  m_actormodel = actor_model; m_TIMESLOT = timeslot; m_MAX_FD = max_fd;
}

void WebServer::set_root(const char *root) {
  if (!root) return;
  if (m_root) free(m_root);
  size_t len = strlen(root) + 1;
  m_root = (char *)malloc(len);
  strcpy(m_root, root);
}

int  WebServer::get_port()       const { return m_port; }
const char *WebServer::get_root() const { return m_root; }
int  WebServer::get_log_write()  const { return m_log_write; }
int  WebServer::get_close_log()  const { return m_close_log; }
int  WebServer::get_actor_model() const { return m_actormodel; }
int  WebServer::get_listenfd()   const { return m_listenfd; }
int  WebServer::get_opt_linger() const { return m_OPT_LINGER; }
int  WebServer::get_sql_num()    const { return m_sql_num; }
int  WebServer::get_thread_num() const { return m_thread_num; }
int  WebServer::get_time_slot()  const { return m_TIMESLOT; }
int  WebServer::get_max_fd()     const { return m_MAX_FD; }
std::string WebServer::get_db_user()   const { return m_user; }
std::string WebServer::get_db_passwd() const { return m_passWord; }
std::string WebServer::get_db_name()   const { return m_databaseName; }

void WebServer::trig_mode() {}

void WebServer::log_write() {
  if (0 == m_close_log) {
    if (1 == m_log_write)
      Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
    else
      Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
  } else {
    Log::get_instance()->m_close_log = 1;
  }
}

void WebServer::sql_pool() {
  m_connPool = connection_pool::GetInstance();
  m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306,
                   m_sql_num, m_close_log);
  m_conn_lock.lock();
  users->initmysql_result(m_connPool);
  m_conn_lock.unlock();
}

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
  } else {
    struct linger tmp = {1, 1};
    setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
  }

  struct sockaddr_in address;
  bzero(&address, sizeof(address));
  address.sin_family      = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port        = htons(m_port);

  int flag = 1;
  setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
  assert(bind(m_listenfd, (struct sockaddr *)&address, sizeof(address)) >= 0);
  assert(listen(m_listenfd, 65535) >= 0);

  utils.init(m_TIMESLOT);
  assert(m_uring.init(512));
  http_conn::m_ring = &m_uring;

  // 信号管道：读端 BLOCKING（io_uring READ 阻塞等待，充当 wakeup）
  assert(socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd) != -1);
  utils.setnonblocking(m_pipefd[1]);  // 写端非阻塞

  utils.addsig(SIGPIPE, SIG_IGN);
  utils.addsig(SIGALRM, utils.sig_handler, false);
  utils.addsig(SIGTERM, utils.sig_handler, false);
  alarm(m_TIMESLOT);

  Utils::u_pipefd[0] = m_pipefd[0];
  Utils::u_pipefd[1] = m_pipefd[1];
  http_conn::s_wakeup_fd = m_pipefd[1];  // worker 唤醒用

  // 提交 multishot accept（一个 SQE，持续接受新连接）+ pipe 阻塞读
  io_uring_sqe *sqe = m_uring.prepare_multishot_accept(m_listenfd, nullptr, nullptr, 0);
  if (sqe) m_uring.sqe_set_data(sqe, &g_accept_marker);
  sqe = m_uring.prepare_recv(m_pipefd[0], m_signal_buf, 1, 0);
  if (sqe) m_uring.sqe_set_data(sqe, &g_wakeup_marker);
  m_uring.submit();
}

// ===================== timer =====================
void WebServer::timer(int connfd, struct sockaddr_in client_address) {
  m_conn_lock.lock();
  users[connfd].init(connfd, client_address, m_root, 0,
                     m_close_log, m_user, m_passWord, m_databaseName);
  m_conn_lock.unlock();

  users_timer[connfd].address = client_address;
  users_timer[connfd].sockfd  = connfd;
  util_timer *t = new util_timer;
  t->user_data = &users_timer[connfd];
  t->cb_func   = cb_func;
  time_t cur   = time(nullptr);
  t->expire    = cur + 3 * m_TIMESLOT;
  users_timer[connfd].timer = t;

  m_timer_lock.lock();
  utils.m_timer_lst.add_timer(t);
  m_timer_lock.unlock();
}

void WebServer::adjust_timer(util_timer *timer) {
  if (!timer) return;
  time_t cur = time(nullptr);
  timer->expire = cur + 3 * m_TIMESLOT;
  m_timer_lock.lock();
  utils.m_timer_lst.adjust_timer(timer);
  m_timer_lock.unlock();
}

void WebServer::deal_timer(util_timer *timer, int sockfd) {
  if (!timer) return;
  timer->cb_func(&users_timer[sockfd]);
  m_timer_lock.lock();
  utils.m_timer_lst.del_timer(timer);
  m_timer_lock.unlock();
}

// ===================== eventLoop =====================
void WebServer::eventLoop() {
  bool timeout     = false;
  bool stop_server = false;

  while (!stop_server) {
    // 1. 先刷 worker 积压
    http_conn::flush_main_queue();

    // 2. 阻塞等待 CQE（pipe 读端阻塞 → worker 写 pipe 唤醒）
    int ret = m_uring.submit_and_wait(1);
    if (ret < 0) {
      LOG_ERROR("io_uring submit_and_wait failed: %d", ret);
      break;
    }

    // 3. 收割 CQE
    io_uring_cqe *cqe;
    while ((cqe = m_uring.peek_cqe()) != nullptr) {
      void *data = m_uring.cqe_get_data(cqe);
      int   res  = m_uring.cqe_get_res(cqe);

      // === accept（multishot：一个 SQE 持续产生 CQE） ===
      if (data == &g_accept_marker) {
        int connfd = res;
        if (connfd >= 0) {
          struct sockaddr_in client_addr;
          socklen_t len = sizeof(client_addr);
          getpeername(connfd, (struct sockaddr *)&client_addr, &len);

          m_conn_lock.lock();
          int user_cnt = http_conn::m_user_count;
          m_conn_lock.unlock();

          if (user_cnt >= m_MAX_FD) {
            utils.show_error(connfd, "Internal server busy");
          } else {
            timer(connfd, client_addr);
            users[connfd].submit_recv();
          }
        }
        // multishot 终止时才重新提交
        if (!(cqe->flags & IORING_CQE_F_MORE)) {
          io_uring_sqe *sqe = m_uring.prepare_multishot_accept(
              m_listenfd, nullptr, nullptr, 0);
          if (sqe) m_uring.sqe_set_data(sqe, &g_accept_marker);
        }
      }
      // === pipe wakeup（worker/signal） ===
      else if (data == &g_wakeup_marker) {
        if (res > 0) {
          for (int i = 0; i < res; ++i) {
            if (m_signal_buf[i] == SIGALRM) timeout = true;
            if (m_signal_buf[i] == SIGTERM) stop_server = true;
          }
        }
        // 可能是 worker 唤醒，刷队列
        http_conn::flush_main_queue();
        // 重新提交 pipe 阻塞读
        io_uring_sqe *sqe = m_uring.prepare_recv(
            m_pipefd[0], m_signal_buf, 1, 0);
        if (sqe) m_uring.sqe_set_data(sqe, &g_wakeup_marker);
      }
      // === RECV ===
      else if (!IoUringEngine::is_send_op(data)) {
        http_conn *conn = static_cast<http_conn *>(
            IoUringEngine::untag(data));
        int sockfd = conn->get_sockfd();
        if (res <= 0) {
          util_timer *t = users_timer[sockfd].timer;
          deal_timer(t, sockfd);
        } else {
          conn->on_recv_done(res);
          if (0 == m_actormodel)
            m_pool->append_p(conn);
          else
            conn->process();
          util_timer *t = users_timer[sockfd].timer;
          adjust_timer(t);
        }
      }
      // === SEND ===
      else {
        http_conn *conn = static_cast<http_conn *>(
            IoUringEngine::untag(data));
        int sockfd = conn->get_sockfd();
        util_timer *t = users_timer[sockfd].timer;
        if (conn->is_linger()) {
          conn->on_send_done();
          adjust_timer(t);
        } else {
          deal_timer(t, sockfd);
        }
      }

      m_uring.cqe_seen(cqe);
    }

    // 4. 定时器心跳
    if (timeout) {
      utils.timer_handler();
      timeout = false;
    }
  }
}
