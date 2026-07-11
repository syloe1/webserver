// ============================================================
// io_uring 最小封装实现 —— 全部透传 liburing C API
// ============================================================
#include "core/io_uring_engine.h"

// ---- 初始化 ----

bool IoUringEngine::init(unsigned entries, unsigned flags, unsigned idle_ms) {
    struct io_uring_params p = {};
    p.flags = flags;
    if (flags & IORING_SETUP_SQPOLL)
        p.sq_thread_idle = idle_ms;  // SQ poll 线程空闲超时（毫秒）

    int ret = io_uring_queue_init_params(entries, &m_ring, &p);
    if (ret < 0)
        return false;
    m_inited = true;
    return true;
}

// ---- 获取裸 SQE ----

io_uring_sqe *IoUringEngine::get_sqe() {
    // 从 SQ 拿一个空闲槽位。队列满时返回 nullptr，调用方需检查
    return io_uring_get_sqe(&m_ring);
}

// ---- SQE 准备 ----

io_uring_sqe *IoUringEngine::prepare_accept(int fd, struct sockaddr *addr,
                                             socklen_t *addrlen,
                                             unsigned flags) {
    io_uring_sqe *sqe = io_uring_get_sqe(&m_ring);
    if (!sqe)
        return nullptr;
    io_uring_prep_accept(sqe, fd, addr, addrlen, flags);
    return sqe;
}

io_uring_sqe *IoUringEngine::prepare_multishot_accept(int fd,
                                                       struct sockaddr *addr,
                                                       socklen_t *addrlen,
                                                       unsigned flags) {
    io_uring_sqe *sqe = io_uring_get_sqe(&m_ring);
    if (!sqe)
        return nullptr;
    io_uring_prep_multishot_accept(sqe, fd, addr, addrlen, flags);
    return sqe;
}

io_uring_sqe *IoUringEngine::prepare_recv(int fd, void *buf, unsigned len,
                                           unsigned flags) {
    io_uring_sqe *sqe = io_uring_get_sqe(&m_ring);
    if (!sqe)
        return nullptr;
    io_uring_prep_recv(sqe, fd, buf, len, flags);
    return sqe;
}

io_uring_sqe *IoUringEngine::prepare_send(int fd, const void *buf,
                                           unsigned len, unsigned flags) {
    io_uring_sqe *sqe = io_uring_get_sqe(&m_ring);
    if (!sqe)
        return nullptr;
    io_uring_prep_send(sqe, fd, buf, len, flags);
    return sqe;
}

io_uring_sqe *IoUringEngine::prepare_close(int fd) {
    io_uring_sqe *sqe = io_uring_get_sqe(&m_ring);
    if (!sqe)
        return nullptr;
    io_uring_prep_close(sqe, fd);
    return sqe;
}

io_uring_sqe *IoUringEngine::prepare_writev(int fd, const struct iovec *iov,
                                             unsigned nr_vecs, off_t offset) {
    io_uring_sqe *sqe = io_uring_get_sqe(&m_ring);
    if (!sqe)
        return nullptr;
    io_uring_prep_writev(sqe, fd, iov, nr_vecs, offset);
    return sqe;
}

// ---- 提交 ----

int IoUringEngine::submit() {
    return io_uring_submit(&m_ring);
}

int IoUringEngine::submit_and_wait(unsigned wait_nr) {
    return io_uring_submit_and_wait(&m_ring, wait_nr);
}

int IoUringEngine::submit_and_wait_timeout(unsigned wait_nr,
                                            unsigned timeout_ms) {
    struct __kernel_timespec ts;
    ts.tv_sec  = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1000000UL;
    struct io_uring_cqe *cqe = nullptr;
    int ret = io_uring_submit_and_wait_timeout(&m_ring, &cqe,
                                                wait_nr, &ts, nullptr);
    return ret;
}

// ---- CQE 收割 ----

io_uring_cqe *IoUringEngine::peek_cqe() {
    io_uring_cqe *cqe = nullptr;
    int ret = io_uring_peek_cqe(&m_ring, &cqe);
    if (ret == 0)
        return cqe;
    return nullptr;
}

void IoUringEngine::cqe_seen(io_uring_cqe *cqe) {
    io_uring_cqe_seen(&m_ring, cqe);
}

// ---- 辅助 ----

void *IoUringEngine::cqe_get_data(const io_uring_cqe *cqe) {
    return io_uring_cqe_get_data(cqe);
}

int IoUringEngine::cqe_get_res(const io_uring_cqe *cqe) {
    return cqe->res; // >=0: 成功读/写的字节数; <0: -errno
}

void IoUringEngine::sqe_set_data(io_uring_sqe *sqe, void *data) {
    io_uring_sqe_set_data(sqe, data);
}

// ---- 生命周期 ----

void IoUringEngine::destroy() {
    if (m_inited) {
        io_uring_queue_exit(&m_ring);
        m_inited = false;
    }
}

IoUringEngine::~IoUringEngine() {
    destroy();
}


