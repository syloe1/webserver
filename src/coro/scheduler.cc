// ============================================================
// Scheduler 实现 —— 每载体线程一个「io_uring + 就绪协程队列」
// ============================================================
#include "coro/scheduler.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "core/log.h"
#include "coro/awaiter.h"
#include "coro/offload.h"
#include "coro/task.h"
#include "net/http_conn.h"

namespace {
// CQE 的三类非连接来源。调度器只收割自己的 ring，所以全局单例标记够用。
char g_accept_marker = 0;
char g_eventfd_marker = 0;
char g_tick_marker = 0;

int64_t monotonic_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
} // namespace

namespace coro {

thread_local Scheduler *t_sched = nullptr;

Scheduler *Scheduler::self() noexcept { return t_sched; }

Scheduler::~Scheduler() {
  if (m_spare_fd >= 0)
    ::close(m_spare_fd);
  if (m_efd >= 0)
    ::close(m_efd);
  if (m_listenfd >= 0)
    ::close(m_listenfd);
}

// ------------------------------------------------------------
// init
// ------------------------------------------------------------
bool Scheduler::init(const Options &opt, int idx, BlockingPool *blocking) {
  m_opt = opt;
  m_idx = idx;
  m_blocking = blocking;

  if (!m_ring.init(opt.ring_entries)) {
    LOG_ERROR("scheduler %d: io_uring_queue_init failed", idx);
    return false;
  }

  // 非 EFD_SEMAPHORE：粘性计数器语义。即使 write 发生在内核看到
  // read SQE 之前，下一次武装的 read 也会立刻读到非零计数并完成，
  // 因此不存在丢唤醒窗口。
  m_efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (m_efd < 0) {
    LOG_ERROR("scheduler %d: eventfd failed: %s", idx, strerror(errno));
    return false;
  }

  m_listenfd =
      ::socket(PF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (m_listenfd < 0) {
    LOG_ERROR("scheduler %d: socket failed: %s", idx, strerror(errno));
    return false;
  }

  int one = 1;
  ::setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (opt.reuse_port) {
    // 必须在 bind 之前设置，否则内核不会把该 socket 并入复用组
    if (::setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) <
        0) {
      LOG_WARN("scheduler %d: SO_REUSEPORT unsupported (%s), "
               "falling back to single-listener mode",
               idx, strerror(errno));
    }
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(static_cast<uint16_t>(opt.port));

  if (::bind(m_listenfd, reinterpret_cast<struct sockaddr *>(&addr),
             sizeof(addr)) < 0) {
    LOG_ERROR("scheduler %d: bind :%d failed: %s", idx, opt.port,
              strerror(errno));
    return false;
  }
  if (::listen(m_listenfd, opt.listen_backlog) < 0) {
    LOG_ERROR("scheduler %d: listen failed: %s", idx, strerror(errno));
    return false;
  }

  // fd 耗尽应急位
  m_spare_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);

  m_inited = true;
  return true;
}

// ------------------------------------------------------------
// io_uring 提交侧
// ------------------------------------------------------------
io_uring_sqe *Scheduler::acquire_sqe(unsigned need) {
  // get_sqe 返回 null 只说明 SQ 环的槽位还没被内核消费，不是致命错误：
  // 先 submit 把已入队的推进内核腾出槽位，再重试。
  for (int i = 0; i < 3; ++i) {
    if (m_ring.sq_space_left() >= need) {
      if (io_uring_sqe *sqe = m_ring.get_sqe())
        return sqe;
    }
    int r = m_ring.submit();
    if (r < 0 && r != -EINTR && r != -EBUSY)
      break;
  }
  return nullptr;
}

bool Scheduler::submit_io() {
  int r = m_ring.submit();
  // -EBUSY（SQPOLL 需唤醒）/ -EINTR 都不是致命错误
  if (r < 0 && r != -EBUSY && r != -EINTR) {
    LOG_ERROR("scheduler %d: io_uring_submit failed: %d", m_idx, r);
    return false;
  }
  return true;
}

void Scheduler::submit_ff(io_uring_sqe *sqe) {
  if (!sqe)
    return;
  m_ring.sqe_set_data(sqe, nullptr); // 结果不关心，CQE 到达后丢弃
  submit_io();
}

bool Scheduler::submit_op(IoAwaiter &op) {
  if (stopping())
    return false;
  io_uring_sqe *sqe = acquire_sqe(1);
  if (!sqe)
    return false;
  op.fill(sqe);
  m_ring.sqe_set_data(sqe, op.record());
  submit_io();
  return true;
}

void Scheduler::cancel_io(const void *user_data) {
  if (!user_data)
    return;
  submit_ff(m_ring.prepare_cancel(user_data, 0));
}

// ------------------------------------------------------------
// 常驻 SQE 武装
// ------------------------------------------------------------
void Scheduler::arm_eventfd() {
  io_uring_sqe *sqe = acquire_sqe(1);
  if (!sqe) {
    m_efd_armed = false;
    return;
  }
  io_uring_prep_read(sqe, m_efd, &m_efd_val, sizeof(m_efd_val), 0);
  m_ring.sqe_set_data(sqe, &g_eventfd_marker);
  m_efd_armed = true;
  submit_io();
}

void Scheduler::arm_tick() {
  const int64_t now = monotonic_ns();
  if (m_next_tick_ns <= now)
    m_next_tick_ns = now;
  m_next_tick_ns += static_cast<int64_t>(m_opt.tick_ms) * 1000000LL;

  m_tick_ts.tv_sec = m_next_tick_ns / 1000000000LL;
  m_tick_ts.tv_nsec = m_next_tick_ns % 1000000000LL;

  io_uring_sqe *sqe = acquire_sqe(1);
  if (!sqe) {
    m_tick_armed = false;
    return;
  }
  io_uring_prep_timeout(sqe, &m_tick_ts, 0, IORING_TIMEOUT_ABS);
  m_ring.sqe_set_data(sqe, &g_tick_marker);
  m_tick_armed = true;
  submit_io();
}

// ------------------------------------------------------------
// 协程投递
// ------------------------------------------------------------
void Scheduler::schedule(std::coroutine_handle<> h) {
  if (!h)
    return;
  {
    // ★ 入队必须在唤醒之前。反过来的话，被唤醒的线程可能 drain 到空
    //   队列又重新睡下，而那次唤醒的"配额"已经被消费掉了 → 真丢唤醒。
    std::lock_guard<std::mutex> g(m_ready_mtx);
    m_ready.push_back(h);
  }
  uint64_t one = 1;
  ssize_t n = ::write(m_efd, &one, sizeof(one));
  (void)n; // 计数器溢出不处理：粘性计数器，下次 read 一定会读到非零
}

void Scheduler::schedule_destroy(std::coroutine_handle<> h, void *owner) {
  if (owner)
    static_cast<http_conn *>(owner)->mark_frame_done();
  m_gc.push_back(GcEntry{h, owner});
}

// ------------------------------------------------------------
// 主循环
// ------------------------------------------------------------
void Scheduler::run() {
  t_sched = this;
  m_next_tick_ns = monotonic_ns();

  LOG_INFO("scheduler %d started on port %d (listenfd=%d)", m_idx, m_opt.port,
           m_listenfd);

  arm_eventfd();
  arm_tick();
  if (io_uring_sqe *sqe =
          m_ring.prepare_multishot_accept(m_listenfd, nullptr, nullptr, 0)) {
    m_ring.sqe_set_data(sqe, &g_accept_marker);
  } else {
    LOG_ERROR("scheduler %d: cannot arm multishot accept", m_idx);
  }
  submit_io();

  while (!stopping()) {
    drain_ready();
    drain_gc();
    // 有就绪协程时不能阻塞。即使这里判断失误（判断后立刻有跨线程投递），
    // eventfd 的常驻 read SQE 也会把 submit_and_wait 立刻唤醒。
    reap_cqes(/*may_block=*/m_ready.empty() && m_gc.empty());
  }

  // 先等在途的阻塞池任务收尾。它们持有指向协程帧的 CompletionRecord，
  // 且回来时还会调一次 schedule()；帧析构之后再被碰就是野指针。
  // 池线程不依赖本调度器推进，所以这里自旋是安全的（只会短暂等待）。
  while (m_offloads.load(std::memory_order_acquire) != 0)
    ::usleep(200);

  // 停机：连接对象析构时会把仍挂起的协程帧一并销毁。
  // 此时 ring 里可能还有指向这些帧的在途 op，但队列马上就拆了，不再收割。
  m_conns.clear();
  m_conn_count = 0;
  m_gc.clear();
  m_ring.destroy();

  LOG_INFO("scheduler %d stopped", m_idx);
  t_sched = nullptr;
}

void Scheduler::stop() noexcept {
  m_stopping.store(true, std::memory_order_relaxed);
  if (m_efd >= 0) {
    uint64_t one = 1;
    ssize_t n = ::write(m_efd, &one, sizeof(one));
    (void)n;
  }
}

// ------------------------------------------------------------
// 调度：跑就绪协程 / 回收帧 / 收割 CQE
// ------------------------------------------------------------
void Scheduler::drain_ready() {
  for (;;) {
    std::coroutine_handle<> h;
    {
      std::lock_guard<std::mutex> g(m_ready_mtx);
      if (m_ready.empty())
        return;
      h = m_ready.front();
      m_ready.pop_front();
    }
    if (h)
      h.resume();
    // 协程可能刚跑到终点，及时回收避免帧堆积
    drain_gc();
  }
}

void Scheduler::drain_gc() {
  if (m_gc.empty())
    return;
  for (const GcEntry &e : m_gc) {
    http_conn *c = static_cast<http_conn *>(e.owner);
    // ★ 顺序不能反：先销毁协程帧，再析构连接对象。
    //   帧内局部量的析构可能访问连接，连接必须活得更久。
    e.h.destroy();
    if (c) {
      // 帧已销毁，清掉句柄防止 ~http_conn 二次 destroy
      c->clear_frame();
      erase_connection(c->get_sockfd());
    }
  }
  m_gc.clear();
}

void Scheduler::reap_cqes(bool may_block) {
  if (may_block) {
    int ret = m_ring.submit_and_wait(1);
    if (ret < 0 && ret != -EINTR && ret != -EBUSY) {
      LOG_ERROR("scheduler %d: submit_and_wait failed: %d (%s)", m_idx, ret,
                strerror(-ret));
      m_stopping.store(true, std::memory_order_relaxed);
      return;
    }
  } else {
    submit_io();
  }

  io_uring_cqe *cqe;
  while ((cqe = m_ring.peek_cqe()) != nullptr) {
    void *data = m_ring.cqe_get_data(cqe);
    const int res = m_ring.cqe_get_res(cqe);

    if (data == &g_accept_marker)
      on_accept(cqe);
    else if (data == &g_eventfd_marker)
      on_eventfd();
    else if (data == &g_tick_marker)
      on_tick();
    else if (data != nullptr) {
      // 连接 I/O 完成：写回 res 后恢复对应协程
      auto *rec = static_cast<CompletionRecord *>(data);
      rec->res = res;
      rec->handle.resume();
    }
    // data == nullptr → fire-and-forget，丢弃

    m_ring.cqe_seen(cqe);
  }
}

// ------------------------------------------------------------
// CQE 分派
// ------------------------------------------------------------
void Scheduler::on_eventfd() {
  // ★ 必须先重新武装，再处理就绪队列。若反序，处理完后若队列为空就会
  //   阻塞在一个没有 eventfd 保护的 submit_and_wait 上，后续跨线程投递
  //   再也唤不醒它。
  arm_eventfd();
  drain_ready();
}

void Scheduler::on_tick() {
  m_tick_armed = false;
  sweep_timeouts();
  arm_tick();
}

void Scheduler::on_accept(const io_uring_cqe *cqe) {
  const int connfd = cqe->res;

  if (connfd >= 0) {
    if (m_conn_count >= static_cast<std::size_t>(m_opt.max_conns)) {
      LOG_WARN("scheduler %d: connection limit %d reached, rejecting fd %d",
               m_idx, m_opt.max_conns, connfd);
      ::close(connfd);
    } else {
      struct sockaddr_in addr;
      memset(&addr, 0, sizeof(addr));
      socklen_t len = sizeof(addr);
      ::getpeername(connfd, reinterpret_cast<struct sockaddr *>(&addr), &len);
      start_connection(connfd, addr);
    }
  } else if (connfd == -EMFILE || connfd == -ENFILE) {
    // fd 表耗尽：关掉预留位腾出一个名额，accept 掉积压连接后立刻补回，
    // 否则 multishot accept 会一直以 -EMFILE 终止而无法推进
    if (m_spare_fd >= 0) {
      ::close(m_spare_fd);
      m_spare_fd = -1;
      int fd = ::accept(m_listenfd, nullptr, nullptr);
      if (fd >= 0)
        ::close(fd);
      m_spare_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    }
    LOG_ERROR("scheduler %d: accept failed: %s", m_idx, strerror(-connfd));
  }

  // multishot：CQE 带 IORING_CQE_F_MORE 表示 SQE 仍生效；
  // 只有它终止时才需要重新武装
  if (!(cqe->flags & IORING_CQE_F_MORE)) {
    if (io_uring_sqe *sqe =
            m_ring.prepare_multishot_accept(m_listenfd, nullptr, nullptr, 0))
      m_ring.sqe_set_data(sqe, &g_accept_marker);
  }
}

// ------------------------------------------------------------
// 连接生命周期
// ------------------------------------------------------------
void Scheduler::start_connection(int connfd, const struct sockaddr_in &addr) {
  auto conn = std::make_unique<http_conn>(this, connfd, addr, m_opt);

  Task<void> t = conn->run();
  // ★ 必须 release：Task 拥有帧，析构时会 destroy，
  //   不解引用就等于把马上要跑的帧当场销毁。
  auto h = t.release();
  h.promise().sched = this;
  h.promise().owner = conn.get();
  h.promise().wait = conn->wait_state();
  conn->adopt_frame(h);

  // ★ 入队要用 h 而不是 conn->frame()：上一行 std::move(conn) 之后
  //   conn 已经是空指针了。
  m_ready.push_back(h);

  m_conns.emplace(connfd, std::move(conn));
  ++m_conn_count;
}

void Scheduler::request_close(http_conn &c) {
  if (c.status() != http_conn::Status::Active)
    return;
  c.set_status(http_conn::Status::Closing);

  switch (c.wait_state()->kind) {
  case WaitState::Io:
    // 取消在途 op：它以 -ECANCELED 完成后恢复协程，
    // 协程看到 Closing 就收摊，自然走到 final_suspend。
    cancel_io(c.wait_state()->cancel_target);
    break;
  case WaitState::Offload:
    // 阻塞池任务无法取消。置了 Closing 就够了，任务返回后协程自己收摊。
    break;
  default:
    // 不在等待却还活着：理论上只在协程已跑完时出现
    if (!c.frame_done() && c.frame())
      c.frame().resume();
    break;
  }
}

void Scheduler::sweep_timeouts() {
  const int64_t now = monotonic_ns();
  std::vector<http_conn *> expired;

  for (auto &kv : m_conns) {
    http_conn &c = *kv.second;
    if (c.status() != http_conn::Status::Active)
      continue;
    const int64_t dl = c.deadline_ns();
    if (dl == 0 || now <= dl)
      continue;
    expired.push_back(&c);
  }

  // 两阶段：request_close 可能 resume 协程，避免在遍历 m_conns 时做这件事
  for (http_conn *c : expired) {
    LOG_INFO("scheduler %d: conn %d idle timeout", m_idx, c->get_sockfd());
    request_close(*c);
  }
}

void Scheduler::erase_connection(int fd) {
  auto it = m_conns.find(fd);
  if (it == m_conns.end())
    return;
  m_conns.erase(it); // ~http_conn：unmap + close(fd)
  if (m_conn_count > 0)
    --m_conn_count;
}

} // namespace coro
