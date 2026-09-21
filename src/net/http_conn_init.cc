// ============================================================
// http_conn 生命周期管理：构造、析构、状态复位
// ============================================================
#include "net/http_conn.h"
#include "db/user_cache.h"
#include "net/socket_tool.h"

#include <cstring>
#include <time.h>
#include <unistd.h>

// ===================== 构造 =====================
http_conn::http_conn(coro::Scheduler *sched, int sockfd,
                     const sockaddr_in &addr,
                     const coro::Scheduler::Options &opt)
    : m_sched(sched),
      m_opt(opt), m_sockfd(sockfd), m_address(addr), m_read_idx(0),
      m_checked_idx(0), m_start_line(0), m_parse_consumed(0), m_write_idx(0),
      m_check_state(CHECK_STATE_REQUESTLINE), m_method(GET), m_content_length(0),
      m_linger(false), m_file_address(nullptr), m_file_len(0), cgi(0) {
  memset(m_read_buf, '\0', READ_BUFFER_SIZE);
  memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
  memset(&m_file_stat, 0, sizeof(m_file_stat));

  // 非阻塞：让 io_uring 走 poll 路径内联完成，而不是把 op 丢给 io-wq
  setnonblocking(m_sockfd);
  arm_deadline();
}

// ===================== 析构 =====================
http_conn::~http_conn() {
  unmap();
  if (m_sockfd >= 0) {
    ::close(m_sockfd);
    m_sockfd = -1;
  }
  // 正常回收路径上 Scheduler::drain_gc 已经 destroy 并 clear_frame()，
  // 这里拿到的是空句柄。只有停机时 m_conns.clear() 才会带着活帧走到
  // 这里，此时由本析构负责销毁。
  if (m_frame) {
    m_frame.destroy();
    m_frame = {};
  }
}

// ===================== 空闲超时 =====================
void http_conn::arm_deadline() noexcept {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  m_deadline_ns = static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec +
                  static_cast<int64_t>(m_opt.idle_timeout_ms) * 1000000LL;
}

// ===================== keep-alive 复位 =====================
// 与 init() 的区别：保留缓冲区里**尚未被本次请求消费**的字节。
// 客户端把两个请求塞进同一个 TCP 段（pipelining）时，第二个请求的
// 字节已经在 m_read_buf 里了，清掉就等于静默丢弃 —— 客户端会一直等
// 第二个响应，服务端却在 recv 上阻塞，直接死锁。
void http_conn::reset_for_next_request() {
  unmap();

  const long consumed = m_parse_consumed;
  const long leftover = (m_read_idx > consumed) ? (m_read_idx - consumed) : 0;
  if (leftover > 0 && consumed > 0)
    memmove(m_read_buf, m_read_buf + consumed, static_cast<size_t>(leftover));

  m_read_idx = leftover;
  m_checked_idx = 0;
  m_start_line = 0;
  m_parse_consumed = 0;
  m_write_idx = 0;
  m_check_state = CHECK_STATE_REQUESTLINE;
  m_method = GET;
  m_url.clear();
  m_version.clear();
  m_host.clear();
  m_content_length = 0;
  m_linger = false;
  cgi = 0;
  m_post_data.clear();
  m_real_file.clear();
  m_file_len = 0;

  memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
  // 只清掉已消费区间，leftover 保留
  if (leftover < READ_BUFFER_SIZE)
    memset(m_read_buf + leftover, '\0',
           static_cast<size_t>(READ_BUFFER_SIZE) - static_cast<size_t>(leftover));
}

// 只读 Getter 全部是头文件里的 inline 定义，这里不再重复实现。
