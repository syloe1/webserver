#ifndef CORO_OFFLOAD_H
#define CORO_OFFLOAD_H

// ============================================================
// 阻塞任务卸载池
//
// MySQL 的 mysql_query / mysql_real_connect 是阻塞 socket I/O，
// 在载体线程上直接调用会把整个 ring 上的所有协程按死。所以把这类
// 调用丢给一个专用线程池，协程侧用 co_await 等待：
//
//   int rc = 0;
//   co_await coro::offload(pool, [&] { rc = mysql_query(mysql, sql); });
//
// 完成时由池线程调用 Scheduler::schedule() 把协程投回它自己的载体
// 线程 —— 这是 M:N 模型里唯一的跨线程恢复点。
// ============================================================

#include <condition_variable>
#include <coroutine>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "coro/scheduler.h"
#include "coro/task.h"

namespace coro {

class BlockingPool {
public:
  explicit BlockingPool(unsigned n);
  ~BlockingPool();

  BlockingPool(const BlockingPool &) = delete;
  BlockingPool &operator=(const BlockingPool &) = delete;

  void post(std::function<void()> fn);
  unsigned size() const noexcept { return m_size; }

private:
  void worker();

  std::vector<std::thread> m_threads;
  std::queue<std::function<void()>> m_queue;
  std::mutex m_mtx;
  std::condition_variable m_cv;
  bool m_stop = false;
  unsigned m_size = 0;
};

// ------------------------------------------------------------
// co_await offload(pool, fn)
// ------------------------------------------------------------
class OffloadAwaiter {
public:
  OffloadAwaiter(BlockingPool *pool, std::function<void()> fn)
      : m_pool(pool), m_fn(std::move(fn)) {}

  bool await_ready() const noexcept { return false; }

  template <class P> bool await_suspend(std::coroutine_handle<P> h) noexcept {
    auto &p = h.promise();
    Scheduler *s = p.sched;
    // 没提交任何东西就返回 false —— 协程立即继续，不会双重恢复
    if (!s || !m_pool || s->stopping())
      return false;

    p.rec.handle = h;
    p.rec.res = 0;
    m_rec = &p.rec;
    m_sched = s;
    m_wait = p.wait;

    if (m_wait) {
      // 阻塞池任务无法被 ASYNC_CANCEL 取消，清扫只能等它自然返回
      m_wait->kind = WaitState::Offload;
      m_wait->cancel_target = nullptr;
    }

    // m_rec 指向协程帧，帧在整个 co_await 期间存活（协程不可能在
    // 挂起中途跑到 final_suspend），因此池线程持有它是安全的。
    CompletionRecord *rec = m_rec;
    Scheduler *sched = m_sched;
    // ★ 先登记再投递：反过来的话，池线程可能在 begin() 之前就把任务跑完
    //   并 end()，计数被减到 -1，停机的等待条件永远不成立。
    sched->offload_begin();
    m_pool->post([this, rec, sched] {
      m_fn();
      rec->res = 0;
      sched->schedule(rec->handle); // 线程安全：入队 + 写 eventfd 唤醒
      // ★ 必须在 schedule 之后：schedule 是最后一次碰 rec 的地方
      sched->offload_end();
    });
    return true;
  }

  void await_resume() const noexcept {
    if (m_wait) {
      m_wait->kind = WaitState::None;
      m_wait->cancel_target = nullptr;
    }
  }

private:
  BlockingPool *m_pool;
  std::function<void()> m_fn;
  CompletionRecord *m_rec = nullptr;
  Scheduler *m_sched = nullptr;
  WaitState *m_wait = nullptr;
};

template <class Fn>
OffloadAwaiter offload(BlockingPool *pool, Fn &&fn) {
  return OffloadAwaiter(pool, std::function<void()>(std::forward<Fn>(fn)));
}

} // namespace coro

#endif // CORO_OFFLOAD_H
