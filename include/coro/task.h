#ifndef CORO_TASK_H
#define CORO_TASK_H

// ============================================================
// Task<T> —— C++20 无栈协程的返回类型
//
// 两种用法，生命周期归属不同：
//
//   1) 根协程（每个连接一个）
//        Task<void> t = run_connection(conn);
//        conn.adopt_frame(t.release());   // ★ 必须 release
//      release() 之后调度器独占帧，由 drain_gc() 销毁。
//      忘记 release 的话，语句结束时 ~Task 会把一个正在跑的帧 destroy 掉。
//
//   2) 嵌套协程（co_await do_request()）
//      Task 临时对象自己拥有帧，await_resume() 之后临时对象析构时销毁。
//      此时 FinalAwaiter 走 continuation 对称转移，不通知调度器。
// ============================================================

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

#include "coro/completion_record.h"

namespace coro {

class Scheduler;

// get_return_object 需要返回 Task<T>，而 Task<T> 又需要 promise_type，
// 互相引用 —— 先前置声明打破循环。
template <class T> class Task;

// ------------------------------------------------------------
// promise 公共部分：调度器、宿主对象、续体、完成记录
// ------------------------------------------------------------
struct TaskPromiseBase {
  // 当前协程所属调度器（载体线程）。挂起/恢复的唯一入口。
  Scheduler *sched = nullptr;

  // 宿主对象（HttpConnection*）。根协程结束时调度器靠它回收连接。
  void *owner = nullptr;

  // 谁在 co_await 我。为空表示根协程。
  std::coroutine_handle<> continuation{};

  // 本次 I/O 的完成记录，地址在整个帧生命周期内恒定，
  // 直接作为 SQE 的 user_data。
  CompletionRecord rec;

  // 指向宿主连接的 WaitState，由 awaiter 在 await_suspend/await_resume
  // 里维护。调度器的超时清扫靠它判断能否用 ASYNC_CANCEL 取消。
  WaitState *wait = nullptr;

  std::exception_ptr exc;

  // 防止重复进入待销毁队列导致二次 destroy
  bool queued_for_destroy = false;

  std::suspend_always initial_suspend() noexcept { return {}; }
  void unhandled_exception() noexcept { exc = std::current_exception(); }

  struct FinalAwaiter {
    bool await_ready() const noexcept { return false; }

    template <class P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) const
        noexcept {
      auto &p = h.promise();
      if (p.continuation) {
        // 嵌套协程：帧归外层 Task 临时对象所有，对称转移到续体。
        return p.continuation;
      }
      // 根协程：交给调度器延迟销毁 —— 不能在自己的栈上销毁自己。
      if (p.sched) {
        p.queued_for_destroy = true;
        p.sched->schedule_destroy(h, p.owner);
      }
      return std::noop_coroutine();
    }

    void await_resume() const noexcept {}
  };

  FinalAwaiter final_suspend() noexcept { return {}; }
};

// get_return_object 必须在具体 promise 上定义：基类不知道 Task<T> 的 T。
// 这里只声明，定义放到 Task<T>/Task<void> 完整之后 —— 函数体里要构造
// Task 对象，而 Task<void> 是显式特化，在类内定义会因为类型不完整而报错。
template <class T> struct TaskPromise : TaskPromiseBase {
  std::optional<T> value;

  Task<T> get_return_object() noexcept;

  template <class U> void return_value(U &&v) {
    value.emplace(std::forward<U>(v));
  }
};

template <> struct TaskPromise<void> : TaskPromiseBase {
  Task<void> get_return_object() noexcept;
  void return_void() noexcept {}
};

// ------------------------------------------------------------
// Task<T>
// ------------------------------------------------------------
template <class T> class Task {
public:
  using promise_type = TaskPromise<T>;
  using handle_t = std::coroutine_handle<promise_type>;

  Task() noexcept = default;
  explicit Task(handle_t h) noexcept : m_h(h) {}

  Task(Task &&o) noexcept : m_h(o.m_h) { o.m_h = {}; }
  Task &operator=(Task &&o) noexcept {
    if (this != &o) {
      if (m_h)
        m_h.destroy();
      m_h = o.m_h;
      o.m_h = {};
    }
    return *this;
  }

  // 拥有帧：未 release 就析构会销毁帧
  ~Task() {
    if (m_h)
      m_h.destroy();
  }

  Task(const Task &) = delete;
  Task &operator=(const Task &) = delete;

  handle_t handle() const noexcept { return m_h; }
  explicit operator bool() const noexcept { return static_cast<bool>(m_h); }

  // 交棒：把帧的所有权让给调用方（根协程启动时必用）
  handle_t release() noexcept {
    handle_t t = m_h;
    m_h = {};
    return t;
  }

  // ---- 作为嵌套 awaitable ----
  //
  // ★ await_suspend 必须是模板：内层协程的 promise 是新建的，
  //   sched/owner/wait 全为空。不从外层 promise 继承的话，内层第一个
  //   co_await 会看到 sched == nullptr 而直接返回 -ECANCELED ——
  //   症状是"所有 do_request 都变成 404"，极难定位。
  bool await_ready() const noexcept { return false; }

  template <class P>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<P> cont) noexcept {
    auto &outer = cont.promise();
    auto &inner = m_h.promise();
    inner.sched = outer.sched;
    inner.owner = outer.owner;
    inner.wait = outer.wait;
    inner.continuation = cont;
    return m_h; // 对称转移，直接开始跑内层协程
  }

  T await_resume() {
    if (m_h.promise().exc)
      std::rethrow_exception(m_h.promise().exc);
    return std::move(*m_h.promise().value);
  }

private:
  handle_t m_h{};
};

// ------------------------------------------------------------
// Task<void> 特化
// ------------------------------------------------------------
template <> class Task<void> {
public:
  using promise_type = TaskPromise<void>;
  using handle_t = std::coroutine_handle<promise_type>;

  Task() noexcept = default;
  explicit Task(handle_t h) noexcept : m_h(h) {}

  Task(Task &&o) noexcept : m_h(o.m_h) { o.m_h = {}; }
  Task &operator=(Task &&o) noexcept {
    if (this != &o) {
      if (m_h)
        m_h.destroy();
      m_h = o.m_h;
      o.m_h = {};
    }
    return *this;
  }

  ~Task() {
    if (m_h)
      m_h.destroy();
  }

  Task(const Task &) = delete;
  Task &operator=(const Task &) = delete;

  handle_t handle() const noexcept { return m_h; }
  explicit operator bool() const noexcept { return static_cast<bool>(m_h); }

  handle_t release() noexcept {
    handle_t t = m_h;
    m_h = {};
    return t;
  }

  // ---- 作为嵌套 awaitable（同 Task<T>，见上方注释）----
  bool await_ready() const noexcept { return false; }

  template <class P>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<P> cont) noexcept {
    auto &outer = cont.promise();
    auto &inner = m_h.promise();
    inner.sched = outer.sched;
    inner.owner = outer.owner;
    inner.wait = outer.wait;
    inner.continuation = cont;
    return m_h;
  }

  void await_resume() {
    if (m_h.promise().exc)
      std::rethrow_exception(m_h.promise().exc);
  }

private:
  handle_t m_h{};
};

// ------------------------------------------------------------
// get_return_object 的延后定义（此时 Task<T> / Task<void> 均已完整）
// ------------------------------------------------------------
template <class T> Task<T> TaskPromise<T>::get_return_object() noexcept {
  return Task<T>{std::coroutine_handle<TaskPromise>::from_promise(*this)};
}

inline Task<void> TaskPromise<void>::get_return_object() noexcept {
  return Task<void>{std::coroutine_handle<TaskPromise>::from_promise(*this)};
}

} // namespace coro

#endif // CORO_TASK_H
