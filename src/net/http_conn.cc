// ============================================================
// http_conn 顶层调度入口 + 类静态变量定义
// 其余实现分散在：
//   http_conn_init.cc    —— 构造/析构/初始化/关闭
//   http_conn_read.cc    —— 非阻塞读 + HTTP报文状态机解析
//   http_conn_write.cc   —— 响应拼接 + 非阻塞发送
//   http_conn_request.cc —— 业务路由/CGI/mmap
// ============================================================
#include "net/http_conn.h"
#include "net/socket_tool.h"

// 静态成员变量定义（全局唯一的epoll fd、在线连接计数）
int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;
locker http_conn::m_count_lock;

// 顶层调度入口：读请求 → 业务处理 → 写响应
void http_conn::process() {
  HTTP_CODE read_ret = process_read();
  if (read_ret == NO_REQUEST) {
    modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
    return;
  }
  bool write_ret = process_write(read_ret);
  if (!write_ret) {
    close_conn();
    return;
  }
  modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
