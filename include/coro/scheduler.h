#ifndef CORO_SCHEDULER_H
#define CORO_SCHEDULER_H

// ============================================================
// Scheduler —— 每载体线程一个协程调度器（M:N 里的 N）
//
// 一个调度器 = 一个独立 io_uring + 一个 eventfd + 一个 listen socket
//            + 一个就绪协程队列 + 一个待销毁帧队列
//
// SO_REUSEPORT：每个载体线程绑自己的 listen socket，内核按四元组哈希
// 分流，因此连接天然归属接受它的线程，无需跨线程交接 connfd。
//
// eventfd 的三个用途：
//   1) 跨线程投递协程后的唤醒（阻塞池完成 DB 调用）
//   2) 停机通知（信号处理函数 write 所有调度器的 eventfd）
//   3) 保证 submit_and_wait 不会永久睡死
// 必须是非 EFD_SEMAPHORE 模式：它是粘性计数器，"write 时内核还没看到
// read SQE" 不会丢唤醒 —— 下一次武装的 read 会立刻看到非零计数。
// ============================================================

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <liburing.h>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/io_uring_engine.h"

class connection_pool;
class http_conn; // 全局作用域：不在 coro 命名空间里

namespace coro {

class IoAwaiter;
class BlockingPool;

class Scheduler {
public:
  struct Options {
    int port = 9006;
    int max_conns = 8192;        // 本调度器的连接上限（MAX_FD / 线程数）
    int tick_ms = 1000;          // 超时扫描周期
    int idle_timeout_ms = 15000; // 连接空闲上限（3 * TIMESLOT）
    int listen_backlog = 65535;
    int ring_entries = 4096;
    bool opt_linger = false;
    bool reuse_port = true;
    int close_log = 0;
    std::string doc_root;
    std::string db_user;
    std::string db_passwd;
    std::string db_name;
    connection_pool *conn_pool = nullptr;
  };

  Scheduler() = default;
  ~Scheduler();

  Scheduler(const Scheduler &) = delete;
  Scheduler &operator=(const Scheduler &) = delete;

  // 建 ring / eventfd / listener，并武装常驻 SQE
  bool init(const Options &opt, int idx, BlockingPool *blocking);

  // 载体线程主循环：跑就绪协程 → 回收帧 → 收割 CQE
  void run();

  // 线程安全停机：置标志 + 写 eventfd 唤醒
  void stop() noexcept;

  int listenfd() const noexcept { return m_listenfd; }
  int index() const noexcept { return m_idx; }
  const Options &options() const noexcept { return m_opt; }
  BlockingPool *blocking_pool() const noexcept { return m_blocking; }
  std::size_t conn_count() const noexcept { return m_conn_count; }
  bool stopping() const noexcept {
    return m_stopping.load(std::memory_order_relaxed);
  }

  // ---- io_uring 提交 ----
  // 被 IoAwaiter::await_suspend 调用：取 SQE → op.fill() → 绑定 user_data
  bool submit_op(IoAwaiter &op);
  // SQ 满时先 submit 腾槽再重试，而不是直接报错
  io_uring_sqe *acquire_sqe(unsigned need = 1);
  bool submit_io();
  // fire-and-forget：user_data = nullptr，CQE 到达后直接丢弃
  void submit_ff(io_uring_sqe *sqe);
  // 取消指定 user_data 对应的在途 op（超时清扫用）
  void cancel_io(const void *user_data);

  // ---- 在途阻塞池任务计数 ----
  // 池线程持有指向协程帧的 CompletionRecord，帧一旦析构它就成了野指针。
  // 停机时必须等计数归零再拆连接，否则会 resume 一个已销毁的帧。
  // 先加计数再 post，池线程先 schedule 再减计数 —— 两侧顺序都不能反。
  void offload_begin() noexcept {
    m_offloads.fetch_add(1, std::memory_order_relaxed);
  }
  void offload_end() noexcept {
    m_offloads.fetch_sub(1, std::memory_order_release);
  }

  // ---- 协程投递 ----
  void schedule(std::coroutine_handle<> h); // 线程安全
  void schedule_destroy(std::coroutine_handle<> h, void *owner);

  static Scheduler *self() noexcept; // thread_local，非调度器线程返回 nullptr

  // ---- 连接生命周期（实现见 scheduler.cc）----
  void start_connection(int connfd, const struct sockaddr_in &addr);
  // 请求关闭：按在途事件类型决定取消 I/O 还是直接唤醒协程
  void request_close(http_conn &c);

private:
  struct GcEntry {
    std::coroutine_handle<> h;
    void *owner;
  };

  void drain_ready();
  void drain_gc();
  void reap_cqes(bool may_block);
  void on_accept(const io_uring_cqe *cqe);
  void on_eventfd();
  void on_tick();
  void arm_eventfd();
  void arm_tick();
  void sweep_timeouts();
  void erase_connection(int fd);

  IoUringEngine m_ring;
  int m_efd = -1;
  int m_listenfd = -1;
  int m_idx = 0;
  Options m_opt;
  BlockingPool *m_blocking = nullptr;

  std::mutex m_ready_mtx;
  std::deque<std::coroutine_handle<>> m_ready;
  std::vector<GcEntry> m_gc;

  std::unordered_map<int, std::unique_ptr<http_conn>> m_conns;
  std::size_t m_conn_count = 0;

  std::atomic<bool> m_stopping{false};
  std::atomic<int> m_offloads{0};

  // 常驻 eventfd read 的落地缓冲，必须活到 CQE 到达
  uint64_t m_efd_val = 0;
  bool m_efd_armed = false;

  // tick 定时器：下一个到期的 CLOCK_MONOTONIC 绝对纳秒。
  // 用 "next += interval" 而不是 "now + interval" 递增，不累积漂移。
  struct __kernel_timespec m_tick_ts {};
  int64_t m_next_tick_ns = 0;
  bool m_tick_armed = false;

  // fd 耗尽的应急备用 fd（EMFILE 时先关它腾出空位，accept 后立刻补回）
  int m_spare_fd = -1;

  bool m_inited = false;
};

} // namespace coro

#endif // CORO_SCHEDULER_H
