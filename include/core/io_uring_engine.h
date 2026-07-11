#ifndef IO_URING_ENGINE_H
#define IO_URING_ENGINE_H

// ============================================================
// io_uring 最小封装 —— 替代 epoll 的异步 I/O 引擎
// 封装 liburing，提供 SQ/CQ 提交与收割的便捷接口
// ============================================================
#include <liburing.h>
#include <sys/socket.h>

class IoUringEngine {
public:
    IoUringEngine() = default;

    // 初始化 io_uring 实例
    // idle_ms: SQPOLL 模式下内核线程空闲多久后休眠（毫秒），0 表示不休眠
    bool init(unsigned entries, unsigned flags = 0, unsigned idle_ms = 2000);

    // ---- SQE 准备（入队，不提交） ----

    // 获取一个空闲 SQE 并填充 IORING_OP_ACCEPT
    io_uring_sqe *prepare_accept(int fd, struct sockaddr *addr,
                                  socklen_t *addrlen, unsigned flags = 0);

    // 多路 accept：一个 SQE 持久生效，每个新连接产生一个 CQE
    io_uring_sqe *prepare_multishot_accept(int fd, struct sockaddr *addr,
                                            socklen_t *addrlen, unsigned flags = 0);

    // 获取一个空闲 SQE 并填充 IORING_OP_RECV
    io_uring_sqe *prepare_recv(int fd, void *buf, unsigned len,
                                unsigned flags = 0);

    // 获取一个空闲 SQE 并填充 IORING_OP_SEND
    io_uring_sqe *prepare_send(int fd, const void *buf, unsigned len,
                                unsigned flags = 0);

    // 获取一个空闲 SQE 并填充 IORING_OP_CLOSE
    io_uring_sqe *prepare_close(int fd);

    // 获取一个空闲 SQE 并填充 IORING_OP_WRITEV
    io_uring_sqe *prepare_writev(int fd, const struct iovec *iov,
                                  unsigned nr_vecs, off_t offset = 0);

    // 获取一个空闲 SQE（纯裸 SQE，让你自定义 opcode）
    io_uring_sqe *get_sqe();

    // ---- 提交 ----

    // 提交所有已入队的 SQE（不等待 CQE）
    int submit();

    // 提交并等待至少 wait_nr 个 CQE 就绪
    int submit_and_wait(unsigned wait_nr = 1);

    // 提交并超时等待（毫秒），用于 main 线程周期性检查 worker 队列
    int submit_and_wait_timeout(unsigned wait_nr, unsigned timeout_ms);

    // ---- CQE 收割 ----

    // 非阻塞收割 CQE 队列头，没就绪返回 nullptr
    io_uring_cqe *peek_cqe();

    // 标记该 CQE 已处理完毕
    void cqe_seen(io_uring_cqe *cqe);

    // ---- 辅助 ----

    // 从 CQE 取 user_data（你在 SQE 里设置的关联数据）
    static void *cqe_get_data(const io_uring_cqe *cqe);

    // 从 CQE 取返回值（>=0 成功字节数，<0 错误码 errno）
    static int cqe_get_res(const io_uring_cqe *cqe);

    // 对标 io_uring_sqe_set_data
    static void sqe_set_data(io_uring_sqe *sqe, void *data);

    // ---- tagged pointer: 用指针低 bit 区分 RECV/SEND ----
    // 指针 8 字节对齐 → bit0 始终为 0 → 偷来标操作类型
    static constexpr uintptr_t TAG_RECV = 0;
    static constexpr uintptr_t TAG_SEND = 1;

    static void *tag_recv(void *ptr) {
        return reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(ptr) | TAG_RECV);
    }
    static void *tag_send(void *ptr) {
        return reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(ptr) | TAG_SEND);
    }
    static void *untag(void *data) {
        return reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(data) & ~1ULL);
    }
    static bool is_send_op(void *data) {
        return reinterpret_cast<uintptr_t>(data) & TAG_SEND;
    }

    // 暴露底层 ring
    io_uring *get_ring() { return &m_ring; }

    // ---- 生命周期 ----

    void destroy();
    ~IoUringEngine();

    // 禁用拷贝
    IoUringEngine(const IoUringEngine &) = delete;
    IoUringEngine &operator=(const IoUringEngine &) = delete;

private:
    io_uring m_ring{};
    bool     m_inited = false;
};

#endif
