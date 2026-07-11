// ============================================================
// http_conn 顶层调度 + 单线程统一提交
// ============================================================
#include "net/http_conn.h"

int http_conn::m_user_count = 0;
IoUringEngine *http_conn::m_ring = nullptr;
locker http_conn::m_count_lock;

// 待提交队列（worker 线程入队，主线程 flush）
locker http_conn::s_sq_lock;
std::list<http_conn *> http_conn::s_sq_queue;
int    http_conn::s_wakeup_fd = -1;

// worker 线程调用：入队 + 写 pipe 唤醒主线程
void http_conn::enqueue_to_main(http_conn *conn) {
  s_sq_lock.lock();
  s_sq_queue.push_back(conn);
  s_sq_lock.unlock();
  // 唤醒主线程（写 1 字节到 pipe，触发 io_uring pipe CQE）
  if (s_wakeup_fd >= 0) {
    char wake_byte = 0;
    write(s_wakeup_fd, &wake_byte, 1);
  }
}

// 主线程调用：批量准备 SQE，一次 submit（收敛系统调用）
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
      io_uring_sqe *sqe = m_ring->prepare_recv(conn->m_sockfd,
          conn->m_read_buf + conn->m_read_idx,
          http_conn::READ_BUFFER_SIZE - conn->m_read_idx, 0);
      if (sqe) {
        m_ring->sqe_set_data(sqe, IoUringEngine::tag_recv(conn));
        has_work = true;
      }
    }

    s_sq_lock.lock();
  }
  s_sq_lock.unlock();

  // 批量提交：所有 SQE 一次 submit
  if (has_work)
    m_ring->submit();
}

// 调度入口（worker 线程执行，只设标记不提交 SQ）
void http_conn::process() {
  HTTP_CODE read_ret = process_read();
  if (read_ret == NO_REQUEST) {
    m_need_recv = true;
    enqueue_to_main(this);
    return;
  }
  bool write_ret = process_write(read_ret);
  if (!write_ret) {
    close_conn();
    return;
  }
  m_need_send = true;
  enqueue_to_main(this);
}

// CQE 回调（主线程，可直接提交）
void http_conn::on_recv_done(int bytes_read) {
  m_read_idx = bytes_read;
}

void http_conn::on_send_done() {
  unmap();
  if (m_linger) {
    init();
    // 准备 RECV SQE，由 submit_and_wait 统一提交
    io_uring_sqe *sqe = m_ring->prepare_recv(m_sockfd,
        m_read_buf, READ_BUFFER_SIZE, 0);
    if (sqe)
      m_ring->sqe_set_data(sqe, IoUringEngine::tag_recv(this));
  }
}

// accept 后的首次 RECV（主线程，SQE 由 submit_and_wait 统一提交）
void http_conn::submit_recv() {
  io_uring_sqe *sqe = m_ring->prepare_recv(m_sockfd,
      m_read_buf, READ_BUFFER_SIZE, 0);
  if (sqe)
    m_ring->sqe_set_data(sqe, IoUringEngine::tag_recv(this));
}
