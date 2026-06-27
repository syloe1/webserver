// ============================================================
// http_conn 生命周期管理：构造、析构、初始化、关闭
// ============================================================
#include "net/http_conn.h"
#include "db/user_cache.h"
#include "net/socket_tool.h"
#include <cstring>

// ===================== 构造 =====================
http_conn::http_conn()
    : m_sockfd(-1), m_read_idx(0), m_checked_idx(0), m_start_line(0),
      m_check_state(CHECK_STATE_REQUESTLINE), m_method(GET),
      m_content_length(0), m_linger(false), m_file_address(nullptr),
      m_iv_count(0), cgi(0), bytes_to_send(0), bytes_have_send(0),
      m_TRIGMode(0), m_close_log(0), m_state(0), timer_flag(0), improv(0),
      mysql(nullptr), m_write_idx(0) {
  memset(m_read_buf, '\0', READ_BUFFER_SIZE);
  memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
}

// ===================== 析构 =====================
http_conn::~http_conn() {
  // 仅关闭socket，不修改全局计数器（析构由delete[]触发）
  if (m_sockfd != -1) {
    close(m_sockfd);
    m_sockfd = -1;
  }
}

// ===================== close_conn =====================
void http_conn::close_conn(bool real_close) {
  if (real_close && (m_sockfd != -1)) {
    printf("close %d\n", m_sockfd);
    removefd(m_epollfd, m_sockfd);
    m_sockfd = -1;
    m_user_count--;
  }
}

// ===================== init(sockfd, ...) 外部初始化 =====================
void http_conn::init(int sockfd, const sockaddr_in &addr, std::string root,
                     int trig_mode, int close_log, std::string sql_user,
                     std::string sql_passwd, std::string sql_db) {
  m_sockfd = sockfd;
  m_address = addr;

  addfd(m_epollfd, sockfd, true, m_TRIGMode);
  m_user_count++;

  doc_root = root;
  m_TRIGMode = trig_mode;
  m_close_log = close_log;

  this->sql_user = sql_user;
  this->sql_passwd = sql_passwd;
  this->sql_name = sql_db;

  init();
}

// ===================== init() 内部状态重置 =====================
void http_conn::init() {
  mysql = NULL;
  bytes_to_send = 0;
  bytes_have_send = 0;
  m_check_state = CHECK_STATE_REQUESTLINE;
  m_linger = false;
  m_method = GET;
  m_url.clear();
  m_version.clear();
  m_content_length = 0;
  m_host.clear();
  m_start_line = 0;
  m_checked_idx = 0;
  m_read_idx = 0;
  m_write_idx = 0;
  cgi = 0;
  m_state = 0;
  timer_flag = 0;
  improv = 0;

  memset(m_read_buf, '\0', READ_BUFFER_SIZE);
  memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
  m_real_file.clear();
}

// ===================== initmysql_result 委托给 UserCache =====================
void http_conn::initmysql_result(connection_pool *connPool) {
  UserCache::getInstance()->load_all_users(connPool);
}

// ===================== 只读Getter实现 =====================
int http_conn::get_sockfd() const { return m_sockfd; }
int http_conn::get_state() const { return m_state; }
std::string http_conn::get_url() const { return m_url; }
std::string http_conn::get_doc_root() const { return doc_root; }
long http_conn::get_content_length() const { return m_content_length; }
bool http_conn::is_linger() const { return m_linger; }
http_conn::METHOD http_conn::get_method() const { return m_method; }
