// ============================================================
// BlockingPool —— 承接阻塞系统调用的专用线程池
// ============================================================
#include "coro/offload.h"

namespace coro {

BlockingPool::BlockingPool(unsigned n) : m_size(n ? n : 1) {
  m_threads.reserve(m_size);
  for (unsigned i = 0; i < m_size; ++i)
    m_threads.emplace_back([this] { worker(); });
}

BlockingPool::~BlockingPool() {
  {
    std::lock_guard<std::mutex> g(m_mtx);
    m_stop = true;
  }
  m_cv.notify_all();
  for (auto &t : m_threads)
    if (t.joinable())
      t.join();
}

void BlockingPool::post(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> g(m_mtx);
    if (m_stop)
      return; // 停机中，直接丢弃；调用方连接也会随之关闭
    m_queue.push(std::move(fn));
  }
  m_cv.notify_one();
}

void BlockingPool::worker() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lk(m_mtx);
      m_cv.wait(lk, [this] { return m_stop || !m_queue.empty(); });
      if (m_queue.empty()) {
        if (m_stop)
          return;
        continue;
      }
      job = std::move(m_queue.front());
      m_queue.pop();
    }
    // 锁外执行：任务里是 mysql_query 这类可能耗时几十毫秒的阻塞调用
    job();
  }
}

} // namespace coro
