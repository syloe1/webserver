#ifndef CORO_COMPLETION_RECORD_H
#define CORO_COMPLETION_RECORD_H

// ============================================================
// 协程完成记录 —— io_uring SQE 的 user_data 指向的对象
//
// 为什么不用 "user_data = &awaiter"：
//   awaiter 是 co_await 表达式在协程帧上的临时对象，挂起期间虽然存活，
//   但编译器可以让**不同 co_await 站点**的临时量复用同一帧偏移
//   （[basic.stc] 只要求生存期不重叠即可复用存储）。一旦复用，一个
//   迟到的 CQE 就会把正在等另一个 op 的协程用错误的 res 唤醒。
//
// 因此 user_data 一律取 &promise.rec —— 它是 promise 的成员，
// 整个帧生命周期内地址恒定，每帧唯一，可直接作为
// IORING_OP_ASYNC_CANCEL 的匹配键。
//
// user_data == nullptr 表示 fire-and-forget（close/cancel 等），
// 调度器收到这类 CQE 只 cqe_seen 不做任何事。
// ============================================================

#include <coroutine>

namespace coro {

struct CompletionRecord {
  std::coroutine_handle<> handle{};
  int res = 0; // >=0 成功字节数；<0 为 -errno
};

// ------------------------------------------------------------
// 连接的在途事件状态
//
// 超时清扫要关闭一个连接时，必须先知道它此刻挂在什么上：
//   Io      —— 挂在 io_uring op 上，可以用 IORING_OP_ASYNC_CANCEL 取消；
//   Offload —— 挂在阻塞池任务上，无法取消，只能置关闭标志等它自然返回；
//   None    —— 不在等待（理论上只会出现在"已结束"状态下）。
//
// 只需要这个状态、不需要在途计数：协程能跑到 final_suspend，
// 就意味着它所有 co_await 都已返回，也就意味着所有引用该帧的 CQE
// 都已被收割。因此"帧结束"天然蕴含"无在途 CQE"。
// ------------------------------------------------------------
struct WaitState {
  enum Kind : uint8_t { None = 0, Io = 1, Offload = 2 };

  Kind kind = None;
  // kind == Io 时指向 &promise.rec，作为 ASYNC_CANCEL 的匹配键
  const void *cancel_target = nullptr;
};

} // namespace coro

#endif // CORO_COMPLETION_RECORD_H
