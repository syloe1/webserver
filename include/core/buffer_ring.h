#ifndef BUFFER_RING_H
#define BUFFER_RING_H

#include <liburing.h>
#include <vector>
#include <cstdint>
#include <cstddef>

constexpr size_t BUF_BLOCK_SIZE = 2048;
constexpr uint16_t BUF_GROUP_ID = 0;

class BufferPool {
public:
    /// @param ring 已初始化的 io_uring 实例
    /// @param buf_count buffer 数量（≤ 32768）
    BufferPool(struct io_uring *ring, uint32_t buf_count);
    ~BufferPool();

    BufferPool(const BufferPool &) = delete;
    BufferPool &operator=(const BufferPool &) = delete;

    /// 归还 buffer 到内核 buffer ring（每次 CQE RECV 完成后必须调用）
    void release(uint32_t buf_id) noexcept;

    /// 按 buf_id 获取 buffer 指针
    void *get_buf_ptr(uint32_t buf_id) const;

    uint32_t total_buf_num() const noexcept { return m_total_buf; }
    size_t single_buf_size() const noexcept { return BUF_BLOCK_SIZE; }
    uint16_t buf_group() const noexcept { return m_group_id; }
    uint32_t get_used_count() const noexcept { return m_used_cnt; }

private:
    struct io_uring         *m_ring;
    const uint32_t           m_total_buf;
    const uint16_t           m_group_id;

    std::vector<char *>      m_buf_ptrs;       // 每个 buffer 的起始地址
    struct io_uring_buf_ring *m_buf_ring = nullptr;
    uint32_t                 m_ring_mask;       // buf_ring 掩码 (nr_bufs - 1)

    uint32_t m_used_cnt = 0;   // 监控：当前 inflight 数量
};

#endif
