// ============================================================
// http_conn 协程主体：读请求 → 解析 → 响应 → keep-alive 循环
//
// 这里就是协程相对回调式 Proactor 的核心差别：I/O 写成同步风格，
// 挂起点由 co_await 表达，调度器负责在 CQE 到达后恢复。
// ============================================================
#include "net/http_conn.h"

#include "coro/awaiter.h"

// ===================== 协程主体 =====================
coro::Task<void> http_conn::run() {
  for (;;) {
    HTTP_CODE ret;

    // process_read 是可重入的：返回 NO_REQUEST 表示"数据还不够，
    // 保存好游标先回去等下一次 recv"，恢复后从 m_checked_idx 继续。
    while ((ret = process_read()) == NO_REQUEST) {
      if (m_read_idx >= READ_BUFFER_SIZE) {
        // 请求头超过读缓冲。静默断开对客户端不友好，回一个 400 再关。
        LOG_WARN("conn %d: request header exceeds %d bytes", m_sockfd,
                 READ_BUFFER_SIZE);
        process_write(BAD_REQUEST);
        co_await send_all();
        co_return;
      }

      arm_deadline();
      const int n = co_await coro::async_recv(
          m_sockfd, m_read_buf + m_read_idx,
          static_cast<unsigned>(READ_BUFFER_SIZE - m_read_idx));
      disarm_deadline();

      if (m_status != Status::Active)
        co_return; // 超时清扫要求关闭
      if (n <= 0)
        co_return; // 对端关闭 / 出错 / 被取消

      m_read_idx += n;
    }

    // 请求已完整解析，do_request 可能挂起（statx / openat / DB 卸载）
    if (ret == GET_REQUEST)
      ret = co_await do_request();

    if (m_status != Status::Active)
      co_return;

    // 访问日志
    const char *method_str = (m_method == POST) ? "POST" : "GET";
    const char *status_str = "200";
    if (ret == BAD_REQUEST)
      status_str = "400";
    else if (ret == FORBIDDEN_REQUEST)
      status_str = "403";
    else if (ret == NO_RESOURCE)
      status_str = "404";
    else if (ret == INTERNAL_ERROR)
      status_str = "500";
    else if (ret == FILE_REQUEST)
      status_str = "200";
    else if (ret == REDIRECT)
      status_str = "302";
    LOG_INFO("%s %s -> %s", method_str, m_url.c_str(), status_str);

    if (!process_write(ret))
      co_return;

    if (!co_await send_all())
      co_return;

    if (m_status != Status::Active)
      co_return;

    if (!m_linger)
      co_return;

    reset_for_next_request();
  }
}

// ===================== 发送 =====================
// 响应 = 响应头（m_write_buf）+ 可选的 mmap 文件体。
// 用一个 while 循环处理 partial write，比原来"原地修改 m_iv 重传"清晰，
// 也顺带把大文件（>2GB）的字节数从 int 换成 size_t 避免溢出。
coro::Task<bool> http_conn::send_all() {
  const size_t head_len = static_cast<size_t>(m_write_idx);
  const size_t total = head_len + m_file_len;
  size_t sent = 0;

  while (sent < total) {
    struct iovec iov[2];
    unsigned n = 0;
    if (sent < head_len)
      iov[n++] = {m_write_buf + sent, head_len - sent};
    if (m_file_address && sent < total) {
      const size_t off = (sent > head_len) ? (sent - head_len) : 0;
      iov[n++] = {m_file_address + off, m_file_len - off};
    }
    if (n == 0)
      break;

    arm_deadline();
    const int r = co_await coro::async_writev(m_sockfd, iov, n);
    disarm_deadline();

    if (m_status != Status::Active)
      co_return false;
    if (r <= 0)
      co_return false;

    sent += static_cast<size_t>(r);
  }

  co_return true;
}

// ===================== unmap =====================
void http_conn::unmap() {
  if (m_file_address) {
    munmap(m_file_address, m_file_len);
    m_file_address = nullptr;
  }
  m_file_len = 0;
}
