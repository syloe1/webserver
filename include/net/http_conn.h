#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "coro/completion_record.h"
#include "coro/scheduler.h"
#include "coro/task.h"
#include "core/log.h"
#include "db/sql_connection_pool.h"

// ============================================================
// http_conn —— 一个连接 = 一个协程
//
// 与 dev 分支（回调式 Proactor）的关键差别：
//   * 不再有 static 共享状态（m_ring / s_sq_queue / m_wakeup_fd ...），
//     每个连接归属一个 Scheduler，调度器独占自己的 io_uring。
//   * 不再有 on_recv_done / on_send_cqe / enqueue_to_main 这套回调，
//     I/O 全部写成 co_await，同步风格。
//   * process_read() 的"数据不够"语义（NO_REQUEST）天然映射成
//     "挂起等下一次 recv"，因此解析状态机几乎原样保留。
// ============================================================
class http_conn {
public:
  // 静态常量
  static const int FILENAME_LEN = 200;
  static const int READ_BUFFER_SIZE = 2048;
  static const int WRITE_BUFFER_SIZE = 2048;

  // HTTP请求方法
  enum METHOD {
    GET = 0,
    POST,
    HEAD,
    PUT,
    DELETE,
    TRACE,
    OPTIONS,
    CONNECT,
    PATH
  };
  // HTTP报文解析阶段状态机（最重要！）
  enum CHECK_STATE {
    CHECK_STATE_REQUESTLINE = 0, // 阶段1：解析请求行（Request-Line）
    CHECK_STATE_HEADER,          // 阶段2：解析请求头（Request Header）
    CHECK_STATE_CONTENT          // 阶段3：解析请求体（Content，POST才会用到）
  };
  // HTTP处理结果码：解析完请求后，服务器返回的处理结果
  enum HTTP_CODE {
    NO_REQUEST,        // 请求不完整，还需要继续读socket数据
    GET_REQUEST,       // 成功拿到一个完整的HTTP请求（GET类）
    BAD_REQUEST,       // 请求报文格式错误 400
    NO_RESOURCE,       // 资源不存在 404
    FORBIDDEN_REQUEST, // 权限不足 403
    FILE_REQUEST,      // 静态文件请求成功，准备返回文件
    INTERNAL_ERROR,    // 服务器内部错误 500
    REDIRECT           // 重定向 3xx
  };
  // 单行解析状态
  enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };

  // 连接状态：超时清扫与帧回收的依据
  enum class Status : uint8_t { Active, Closing, Dead };

public:
  http_conn(coro::Scheduler *sched, int sockfd, const sockaddr_in &addr,
            const coro::Scheduler::Options &opt);
  ~http_conn();

  // 禁止拷贝，持有fd、协程帧、mmap资源不可复制
  http_conn(const http_conn &) = delete;
  http_conn &operator=(const http_conn &) = delete;

  // ---- 协程主体：读请求 → 解析 → 响应 → （keep-alive 则循环）----
  coro::Task<void> run();

  // ---- 协程帧管理（由 Scheduler 调用）----
  void adopt_frame(std::coroutine_handle<> h) noexcept { m_frame = h; }
  std::coroutine_handle<> frame() const noexcept { return m_frame; }
  // 帧已由 Scheduler::drain_gc 销毁，清掉句柄防止析构时二次 destroy
  void clear_frame() noexcept { m_frame = {}; }
  bool frame_done() const noexcept { return m_frame_done; }
  void mark_frame_done() noexcept { m_frame_done = true; }

  coro::WaitState *wait_state() noexcept { return &m_wait; }

  Status status() const noexcept { return m_status; }
  void set_status(Status s) noexcept { m_status = s; }

  // 空闲超时：0 表示不设限
  int64_t deadline_ns() const noexcept { return m_deadline_ns; }
  void arm_deadline() noexcept;
  void disarm_deadline() noexcept { m_deadline_ns = 0; }

  // ---- Getter ----
  int get_sockfd() const noexcept { return m_sockfd; }

private:
  // keep-alive 复用连接：保留缓冲区里尚未消费的字节（HTTP pipelining）
  void reset_for_next_request();

  // ---- 解析状态机（可重入：返回 NO_REQUEST 表示需要更多数据）----
  // 保持同步：它只碰内存，不碰任何 I/O，热路径上零协程开销。
  // "需要更多数据" 由调用方 run() 翻译成一次 co_await async_recv。
  HTTP_CODE process_read();
  HTTP_CODE parse_request_line(const char *text);
  HTTP_CODE parse_headers(const char *text);
  HTTP_CODE parse_content(const char *text);
  LINE_STATUS parse_line();
  char *get_line() { return &m_read_buf[m_start_line]; }

  // ---- 路由业务 ----
  // 异步 statx + openat；mmap 保持同步（靠缺页惰性加载）
  coro::Task<HTTP_CODE> do_request();
  // 阻塞的 MySQL 调用，经阻塞池卸载
  coro::Task<HTTP_CODE> do_cgi_login(std::string name, std::string password,
                                     char url_suffix);

  // ---- 响应 ----
  bool process_write(HTTP_CODE ret);
  coro::Task<bool> send_all();
  void unmap();

  bool add_response(const char *format, ...);
  bool add_content(const char *content);
  bool add_status_line(int status, const char *title);
  bool add_headers(long long content_length);
  bool add_content_type();
  bool add_content_length(long long content_length);
  bool add_linger();
  bool add_blank_line();

private:
  // 归属的调度器与运行配置
  coro::Scheduler *m_sched;
  coro::Scheduler::Options m_opt;

  // 协程帧（非拥有：销毁由 Scheduler::drain_gc 负责）
  std::coroutine_handle<> m_frame{};
  bool m_frame_done = false;

  // 在途事件状态，超时清扫据此决定能否 ASYNC_CANCEL
  coro::WaitState m_wait{};
  Status m_status = Status::Active;

  // 空闲超时的 CLOCK_MONOTONIC 绝对纳秒，0 = 不限
  int64_t m_deadline_ns = 0;

  // TCP套接字
  int m_sockfd;
  sockaddr_in m_address;

  // 读缓冲与解析游标
  char m_read_buf[READ_BUFFER_SIZE];
  long m_read_idx;
  long m_checked_idx;
  int m_start_line;
  // 本次请求在 m_read_buf 中实际消费到的偏移，供 keep-alive 复位保留尾部
  long m_parse_consumed;

  // 响应缓冲区
  char m_write_buf[WRITE_BUFFER_SIZE];
  int m_write_idx;

  // HTTP解析状态
  CHECK_STATE m_check_state;
  METHOD m_method;

  // 请求元信息
  std::string m_real_file;
  std::string m_url;
  std::string m_version;
  std::string m_host;
  long m_content_length;
  bool m_linger;

  // 文件映射
  struct statx m_file_stat;
  char *m_file_address;
  size_t m_file_len;

  // POST CGI标记
  int cgi;
  std::string m_post_data;
};

#endif
