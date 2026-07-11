#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <liburing.h>
#include <errno.h>

#define RING_ENTRIES 128
#define BUFFER_CNT 8
#define BUFFER_SIZE 4096
#define TEST_LISTEN_PORT 18888

char *buf_storage[BUFFER_CNT];
struct io_uring_buf_ring *buf_ring;
uint16_t g_bgid = 0;

static int setup_buffer_ring(struct io_uring *ring)
{
    // 分配buffer内存
    for (int i = 0; i < BUFFER_CNT; i++) {
        buf_storage[i] = malloc(BUFFER_SIZE);
        if (!buf_storage[i]) {
            perror("malloc buffer fail");
            return -1;
        }
    }

    // 分配buffer ring
    size_t ring_sz = BUFFER_CNT * sizeof(struct io_uring_buf);
    buf_ring = malloc(ring_sz);
    if (!buf_ring) {
        perror("malloc buf ring fail");
        return -1;
    }
    memset(buf_ring, 0, ring_sz);

    // 填充buffer条目
    for (int bid = 0; bid < BUFFER_CNT; bid++) {
        struct io_uring_buf *entry = &buf_ring->bufs[bid];
        entry->addr = (uint64_t)(unsigned long)buf_storage[bid];
        entry->len = BUFFER_SIZE;
        entry->bid = bid;
    }

    // Buffer Ring 专用注册结构体 & API
    struct io_uring_buf_ring_reg br_reg = {
        .ring = (uint64_t)(unsigned long)buf_ring,
        .nr_bufs = BUFFER_CNT,
        .bgid = g_bgid
    };

    // 正确API：io_uring_register_buf_ring
    int ret = io_uring_register_buf_ring(ring, &br_reg, 0);
    if (ret < 0) {
        perror("io_uring_register_buf_ring failed");
        fprintf(stderr, "内核/liburing 不支持 buffer ring select\n");
        return -1;
    }
    printf("✅ buffer ring 注册成功，buffer组ID: %d\n", g_bgid);
    return 0;
}

static int create_listen_socket()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(TEST_LISTEN_PORT),
        .sin_addr.s_addr = INADDR_ANY
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    listen(fd, 1);
    printf("监听端口: %d，nc 127.0.0.1 %d 测试\n", TEST_LISTEN_PORT, TEST_LISTEN_PORT);
    return fd;
}

static int submit_recv_with_buffer_select(struct io_uring *ring, int client_fd)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe) {
        fprintf(stderr, "SQE耗尽\n");
        return -1;
    }

    io_uring_prep_recv(sqe, client_fd, NULL, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = g_bgid;
    sqe->user_data = (uint64_t)client_fd;

    int ret = io_uring_submit(ring);
    if (ret <= 0) {
        perror("io_uring_submit");
        return -1;
    }
    return 0;
}

// 归还buffer到buffer ring，必须调用
static void return_buffer_to_ring(uint16_t bid)
{
    io_uring_buf_ring_add(buf_ring, buf_storage[bid], BUFFER_SIZE, bid, 0, BUFFER_CNT);
    io_uring_buf_ring_advance(buf_ring, 1);
}

int main()
{
    struct io_uring ring;
    int ret = io_uring_queue_init(RING_ENTRIES, &ring, 0);
    if (ret < 0) {
        perror("io_uring_queue_init");
        return 1;
    }

    if (setup_buffer_ring(&ring) != 0)
        goto cleanup_ring;

    int listen_fd = create_listen_socket();
    if (listen_fd < 0)
        goto cleanup_buf;

    for (;;) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd = accept4(listen_fd, (struct sockaddr *)&cli_addr, &cli_len, SOCK_NONBLOCK);
        if (client_fd < 0) {
            perror("accept4");
            continue;
        }
        printf("新连接 fd=%d\n", client_fd);

        if (submit_recv_with_buffer_select(&ring, client_fd) != 0) {
            close(client_fd);
            continue;
        }

        struct io_uring_cqe *cqe;
        ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) {
            perror("io_uring_wait_cqe 无CQE返回，内核不支持buffer select");
            close(client_fd);
            continue;
        }

        int res = cqe->res;
        uint16_t bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
        printf("✅ CQE正常返回 | recv长度=%d | bufferID=%d\n", res, bid);

        if (res > 0) {
            printf("收到数据: %.*s\n", res, buf_storage[bid]);
        } else if (res == 0) {
            printf("客户端关闭连接 fd=%d\n", (int)cqe->user_data);
        } else {
            fprintf(stderr, "recv 错误: %s\n", strerror(-res));
        }

        // 核心修复：用完buffer必须归还
        return_buffer_to_ring(bid);

        io_uring_cqe_seen(&ring, cqe);
        close(client_fd);
    }

close_listen:
    close(listen_fd);
cleanup_buf:
    free(buf_ring);
    for (int i = 0; i < BUFFER_CNT; i++)
        free(buf_storage[i]);
cleanup_ring:
    io_uring_queue_exit(&ring);
    return 0;
}
