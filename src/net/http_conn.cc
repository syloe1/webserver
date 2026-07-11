// ============================================================
// http_conn 顶层调度 + 单线程统一提交
// ============================================================
#include "net/http_conn.h"

int http_conn::m_user_count = 0;
IoUringEngine *http_conn::m_ring = nullptr;
BufferPool   *http_conn::s_buf_pool = nullptr;
locker http_conn::m_count_lock;

locker http_conn::s_sq_lock;
std::list<http_conn *> http_conn::s_sq_queue;
int    http_conn::s_wakeup_fd = -1;

// 准备 RECV SQE（buffer ring 或普通模式）
void http_conn::submit_recv() {
  io_uring_sqe *sqe = nullptr;
  if (s_buf_pool) {
    // buffer ring 模式：内核自动选 buffer，len 必须为 0
    sqe = m_ring->prepare_recv(m_sockfd, nullptr, 0, 0);
    if (sqe) {
      sqe->buf_group = BUF_GROUP_ID;
      sqe->flags |= IOSQE_BUFFER_SELECT;
    }
  } else {
    // 普通模式：用 m_read_buf
    sqe = m_ring->prepare_recv(m_sockfd, m_read_buf, READ_BUFFER_SIZE, 0);
  }
  if (sqe)
    m_ring->sqe_set_data(sqe, IoUringEngine::tag_recv(this));
  m_ring->submit();
}

// worker 线程调用：入队 + 写 pipe 唤醒主线程
void http_conn::enqueue_to_main(http_conn *conn) {
  s_sq_lock.lock();
  s_sq_queue.push_back(conn);
  s_sq_lock.unlock();
  if (s_wakeup_fd >= 0) {
    char wake_byte = 0;
    write(s_wakeup_fd, &wake_byte, 1);
  }
}

// 主线程调用：批量准备 SQE，一次 submit
void http_conn::flush_main_queue() {
  bool has_work = false;
  s_sq_lock.lock();
  while (!s_sq_queue.empty()) {
    http_conn *conn = s_sq_queue.front();
    s_sq_queue.pop_front();
    s_sq_lock.unlock();

    if (conn->m_need_send) {
      conn->m_need_send = false;
      io_uring_sqe *sqe = nullptr;
      if (conn->m_iv_count == 2) {
        sqe = m_ring->prepare_writev(conn->m_sockfd, conn->m_iv,
                                      conn->m_iv_count, 0);
      } else {
        sqe = m_ring->prepare_send(conn->m_sockfd, conn->m_write_buf,
                                    conn->m_write_idx, 0);
      }
      if (sqe) {
        m_ring->sqe_set_data(sqe, IoUringEngine::tag_send(conn));
        has_work = true;
      }
    }
    if (conn->m_need_recv) {
      conn->m_need_recv = false;
      conn->submit_recv();
      has_work = true;
    }

    s_sq_lock.lock();
  }
  s_sq_lock.unlock();

  if (has_work)
    m_ring->submit();
}

void http_conn::process() {
  HTTP_CODE read_ret = process_read();
  if (read_ret == NO_REQUEST) {
    m_need_recv = true;
    enqueue_to_main(this);
    return;
  }
  // 请求日志：METHOD URL -> STATUS
  const char *method_str = (m_method == POST) ? "POST" : "GET";
  const char *status_str = "200";
  if (read_ret == BAD_REQUEST)      status_str = "400";
  else if (read_ret == FORBIDDEN_REQUEST) status_str = "403";
  else if (read_ret == NO_RESOURCE) status_str = "404";
  else if (read_ret == INTERNAL_ERROR)   status_str = "500";
  else if (read_ret == FILE_REQUEST)     status_str = "200";
  LOG_INFO("%s %s -> %s", method_str, m_url.c_str(), status_str);

  bool write_ret = process_write(read_ret);
  if (!write_ret) {
    close_conn();
    return;
  }
  m_need_send = true;
  enqueue_to_main(this);
}

// RECV CQE 回调：buffer ring 模式从 pool 拷贝，普通模式数据已在 m_read_buf
void http_conn::on_recv_done(int bytes_read, int buf_id) {
  if (buf_id >= 0 && s_buf_pool) {
    void *src = s_buf_pool->get_buf_ptr(buf_id);
    if (src) {
      size_t copy_len = (bytes_read > 0)
          ? std::min((size_t)bytes_read, (size_t)READ_BUFFER_SIZE - m_read_idx)
          : 0;
      if (copy_len > 0)
        memcpy(m_read_buf + m_read_idx, src, copy_len);
    }
    s_buf_pool->release(buf_id);
  }
  m_read_idx += bytes_read;
}

void http_conn::on_send_done() {
  unmap();
  if (m_linger) {
    init();
    // keep-alive: 立即提交 RECV SQE，防止 kernel 来不及接收下个请求
    io_uring_sqe *sqe = nullptr;
    if (s_buf_pool) {
      sqe = m_ring->prepare_recv(m_sockfd, nullptr, 0, 0);
      if (sqe) {
        sqe->buf_group = BUF_GROUP_ID;
        sqe->flags |= IOSQE_BUFFER_SELECT;
      }
    } else {
      sqe = m_ring->prepare_recv(m_sockfd, m_read_buf, READ_BUFFER_SIZE, 0);
    }
    if (sqe)
      m_ring->sqe_set_data(sqe, IoUringEngine::tag_recv(this));
    m_ring->submit();
  }
}

// SEND CQE 处理：全部发送完返回 true，部分发送更新 iovec 重传
bool http_conn::on_send_cqe(int bytes_sent) {
  if (bytes_sent <= 0) return false;
  if (bytes_sent >= bytes_to_send) return true;
  // 部分发送：更新 iovec 继续
  bytes_have_send += bytes_sent;
  bytes_to_send   -= bytes_sent;
  if ((size_t)bytes_have_send >= m_iv[0].iov_len) {
    m_iv[0].iov_len = 0;
    m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
    m_iv[1].iov_len   = bytes_to_send;
  } else {
    m_iv[0].iov_base = m_write_buf + bytes_have_send;
    m_iv[0].iov_len -= bytes_sent;
  }
  io_uring_sqe *sqe = m_ring->prepare_writev(m_sockfd, m_iv, m_iv_count, 0);
  if (sqe) m_ring->sqe_set_data(sqe, IoUringEngine::tag_send(this));
  m_ring->submit();
  return false;
}
