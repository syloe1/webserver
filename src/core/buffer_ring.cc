#include "core/buffer_ring.h"
#include <cstring>
#include <stdexcept>
#include <stdlib.h>
#include <linux/io_uring.h>
#include <sys/mman.h>

BufferPool::BufferPool(struct io_uring *ring, uint32_t buf_count)
    : m_ring(ring), m_total_buf(buf_count), m_group_id(BUF_GROUP_ID)
{
    if (!ring || buf_count == 0)
        throw std::invalid_argument("invalid ring or buf_count");

    // 1. 为每个 buffer 分配内存
    m_buf_ptrs.resize(buf_count, nullptr);
    for (uint32_t i = 0; i < buf_count; i++) {
        m_buf_ptrs[i] = (char *)malloc(BUF_BLOCK_SIZE);
        if (!m_buf_ptrs[i]) {
            for (uint32_t j = 0; j < i; j++)
                free(m_buf_ptrs[j]);
            throw std::runtime_error("BufferPool malloc failed");
        }
    }

    // 2. 分配 buffer ring（页对齐）
    size_t ring_sz = buf_count * sizeof(struct io_uring_buf);
    if (posix_memalign((void **)&m_buf_ring, 4096, ring_sz) != 0) {
        for (uint32_t i = 0; i < buf_count; i++)
            free(m_buf_ptrs[i]);
        throw std::runtime_error("BufferPool posix_memalign for buf_ring failed");
    }
    memset(m_buf_ring, 0, ring_sz);

    // 3. 填充 buffer ring
    for (uint32_t i = 0; i < buf_count; i++) {
        struct io_uring_buf *entry = &m_buf_ring->bufs[i];
        entry->addr = (__u64)(unsigned long)m_buf_ptrs[i];
        entry->len  = BUF_BLOCK_SIZE;
        entry->bid  = i;
    }

    // 4. 注册到内核（手动构造 reg struct，绕过 liburing bug）
    struct io_uring_buf_reg reg = {};
    reg.ring_addr    = (__u64)(unsigned long)m_buf_ring;
    reg.ring_entries = buf_count;
    reg.bgid         = m_group_id;
    reg.flags        = 0;

    int ret = io_uring_register_buf_ring(m_ring, &reg, 0);
    if (ret < 0) {
        free(m_buf_ring);
        m_buf_ring = nullptr;
        for (uint32_t i = 0; i < buf_count; i++)
            free(m_buf_ptrs[i]);
        throw std::runtime_error("io_uring_register_buf_ring failed, ret: "
                                 + std::to_string(ret));
    }

    // 5. 通知内核所有 buffer 已就绪
    io_uring_buf_ring_advance(m_buf_ring, buf_count);

    m_ring_mask = io_uring_buf_ring_mask(buf_count);
    m_used_cnt  = 0;
}

BufferPool::~BufferPool() {
    if (m_ring && m_buf_ring) {
        io_uring_unregister_buf_ring(m_ring, m_group_id);
    }
    free(m_buf_ring);
    m_buf_ring = nullptr;
    for (size_t i = 0; i < m_buf_ptrs.size(); i++)
        free(m_buf_ptrs[i]);
    m_buf_ptrs.clear();
}

void BufferPool::release(uint32_t buf_id) noexcept {
    if (buf_id >= m_total_buf || !m_buf_ring)
        return;
    io_uring_buf_ring_add(m_buf_ring, m_buf_ptrs[buf_id],
                          BUF_BLOCK_SIZE, buf_id, m_ring_mask, 0);
    io_uring_buf_ring_advance(m_buf_ring, 1);
    if (m_used_cnt > 0) m_used_cnt--;
}

void *BufferPool::get_buf_ptr(uint32_t buf_id) const {
    if (buf_id >= m_total_buf)
        return nullptr;
    return m_buf_ptrs[buf_id];
}
