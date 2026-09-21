#ifndef CORO_AWAITER_H
#define CORO_AWAITER_H

// ============================================================
// io_uring awaiter —— 把一次异步 I/O 变成 co_await 表达式
//
//   int n = co_await coro::async_recv(fd, buf, len);
//
// await_suspend 返回 bool 的语义（[expr.await]）：
//   返回 true  → 已挂起，控制权交回调度器
//   返回 false → 不挂起，协程在同一栈上继续，紧接着调 await_resume()
//
// ★ 返回 false 的合法前提是"从未拿到 SQE"。一旦 op 已提交就必须挂起，
//   否则该 op 完成后会 resume 一个正在运行或已结束的协程（双重恢复）。
//   因此只有两种情况返回 false：调度器正在停机、SQ 实在取不到槽位。
//   这两种情况 await_resume() 返回 -ECANCELED 让协程自己收摊。
// ============================================================

#include <cerrno>
#include <coroutine>
#include <fcntl.h>
#include <liburing.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include "coro/completion_record.h"
#include "coro/scheduler.h"

namespace coro {

// ------------------------------------------------------------
// awaiter 基类
// ------------------------------------------------------------
class IoAwaiter {
public:
  virtual ~IoAwaiter() = default;

  bool await_ready() const noexcept { return false; }

  template <class P> bool await_suspend(std::coroutine_handle<P> h) noexcept {
    auto &p = h.promise();
    p.rec.handle = h;
    p.rec.res = 0;
    m_rec = &p.rec;
    m_wait = p.wait;

    Scheduler *s = p.sched;
    if (!s || s->stopping() || !s->submit_op(*this)) {
      m_rec = nullptr; // 未提交 → await_resume() 报 -ECANCELED
      return false;
    }
    // 提交成功后才置在途标志：清扫看到 Io 才会去 cancel，
    // 而 cancel 的目标必须是一个真实存在的 op。
    if (m_wait) {
      m_wait->kind = WaitState::Io;
      m_wait->cancel_target = m_rec;
    }
    return true;
  }

  int await_resume() const noexcept {
    if (m_wait) {
      m_wait->kind = WaitState::None;
      m_wait->cancel_target = nullptr;
    }
    return m_rec ? m_rec->res : -ECANCELED;
  }

  CompletionRecord *record() const noexcept { return m_rec; }

  // 用具体 op 填充 SQE。所有 op 都只占 1 个 SQE，因此由 Scheduler
  // 先取好 SQE 再回调本函数，避免"取了 SQE 才发现要更多"的泄漏。
  virtual void fill(io_uring_sqe *sqe) noexcept = 0;

protected:
  CompletionRecord *m_rec = nullptr;
  WaitState *m_wait = nullptr;
};

// ------------------------------------------------------------
// 具体 awaiter
// ------------------------------------------------------------
class RecvAwaiter : public IoAwaiter {
public:
  RecvAwaiter(int fd, void *buf, unsigned len, int flags = 0) noexcept
      : m_fd(fd), m_buf(buf), m_len(len), m_flags(flags) {}

  void fill(io_uring_sqe *sqe) noexcept override {
    io_uring_prep_recv(sqe, m_fd, m_buf, m_len, m_flags);
  }

private:
  int m_fd;
  void *m_buf;
  unsigned m_len;
  int m_flags;
};

class SendAwaiter : public IoAwaiter {
public:
  SendAwaiter(int fd, const void *buf, unsigned len, int flags = 0) noexcept
      : m_fd(fd), m_buf(buf), m_len(len), m_flags(flags) {}

  void fill(io_uring_sqe *sqe) noexcept override {
    io_uring_prep_send(sqe, m_fd, m_buf, m_len, m_flags);
  }

private:
  int m_fd;
  const void *m_buf;
  unsigned m_len;
  int m_flags;
};

class WritevAwaiter : public IoAwaiter {
public:
  // iovec 数组必须存活到 co_await 结束 —— 通常是调用方协程帧里的局部量
  WritevAwaiter(int fd, const struct iovec *iov, unsigned nr,
                off_t offset = 0) noexcept
      : m_fd(fd), m_iov(iov), m_nr(nr), m_offset(offset) {}

  void fill(io_uring_sqe *sqe) noexcept override {
    io_uring_prep_writev(sqe, m_fd, m_iov, m_nr, m_offset);
  }

private:
  int m_fd;
  const struct iovec *m_iov;
  unsigned m_nr;
  off_t m_offset;
};

class OpenAtAwaiter : public IoAwaiter {
public:
  OpenAtAwaiter(int dfd, const char *path, int flags,
                mode_t mode = 0) noexcept
      : m_dfd(dfd), m_path(path), m_flags(flags), m_mode(mode) {}

  void fill(io_uring_sqe *sqe) noexcept override {
    io_uring_prep_openat(sqe, m_dfd, m_path, m_flags, m_mode);
  }

private:
  int m_dfd;
  const char *m_path;
  int m_flags;
  mode_t m_mode;
};

class StatxAwaiter : public IoAwaiter {
public:
  // statxbuf 同样必须存活到 co_await 结束
  StatxAwaiter(int dfd, const char *path, int flags, unsigned mask,
               struct statx *buf) noexcept
      : m_dfd(dfd), m_path(path), m_flags(flags), m_mask(mask), m_buf(buf) {}

  void fill(io_uring_sqe *sqe) noexcept override {
    io_uring_prep_statx(sqe, m_dfd, m_path, m_flags, m_mask, m_buf);
  }

private:
  int m_dfd;
  const char *m_path;
  int m_flags;
  unsigned m_mask;
  struct statx *m_buf;
};

// ------------------------------------------------------------
// 构造辅助：让调用点写成 co_await coro::async_recv(...)
// ------------------------------------------------------------
inline RecvAwaiter async_recv(int fd, void *buf, unsigned len, int flags = 0) {
  return RecvAwaiter(fd, buf, len, flags);
}

inline SendAwaiter async_send(int fd, const void *buf, unsigned len,
                              int flags = 0) {
  return SendAwaiter(fd, buf, len, flags);
}

inline WritevAwaiter async_writev(int fd, const struct iovec *iov, unsigned nr,
                                  off_t offset = 0) {
  return WritevAwaiter(fd, iov, nr, offset);
}

inline OpenAtAwaiter async_openat(int dfd, const char *path, int flags,
                                  mode_t mode = 0) {
  return OpenAtAwaiter(dfd, path, flags, mode);
}

inline StatxAwaiter async_statx(int dfd, const char *path, int flags,
                                unsigned mask, struct statx *buf) {
  return StatxAwaiter(dfd, path, flags, mask, buf);
}

} // namespace coro

#endif // CORO_AWAITER_H
