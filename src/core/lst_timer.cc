#include "core/lst_timer.h"
#include "net/http_conn.h"
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <signal.h>
#include <unistd.h>

// 静态私有全局资源定义
int Utils::u_pipefd[2] = {0, 0};
int Utils::u_epollfd = 0;

// ===================== sort_timer_lst 实现 =====================
sort_timer_lst::sort_timer_lst() : head(nullptr), tail(nullptr) {}

sort_timer_lst::~sort_timer_lst() {
  locker_guard guard(lst_lock);
  util_timer *tmp = head;
  while (tmp) {
    util_timer *del = tmp;
    tmp = tmp->next;
    // user_data 指向 WebServer::users_timer 数组元素，
    // 生命周期由数组delete[]统一管理，不可单独delete
    del->user_data = nullptr;
    delete del;
  }
  head = nullptr;
  tail = nullptr;
}

void sort_timer_lst::add_timer(util_timer *timer) {
  if (!timer)
    return;
  locker_guard guard(lst_lock);
  if (!head) {
    head = tail = timer;
    return;
  }
  if (timer->expire < head->expire) {
    timer->next = head;
    head->prev = timer;
    head = timer;
    return;
  }
  add_timer(timer, head);
}

void sort_timer_lst::adjust_timer(util_timer *timer) {
  if (!timer)
    return;
  locker_guard guard(lst_lock);
  util_timer *tmp = timer->next;
  // 下一个节点为空 或 当前超时小于后继，无需调整
  if (!tmp || timer->expire < tmp->expire)
    return;

  // 摘下当前节点
  if (timer == head) {
    head = head->next;
    head->prev = nullptr;
  } else {
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
  }
  timer->prev = nullptr;
  timer->next = nullptr;
  add_timer(timer, head);
}

void sort_timer_lst::del_timer(util_timer *timer) {
  if (!timer)
    return;
  locker_guard guard(lst_lock);
  // 链表仅一个节点
  if (timer == head && timer == tail) {
    delete timer;
    head = nullptr;
    tail = nullptr;
    return;
  }
  // 头节点
  if (timer == head) {
    head = head->next;
    head->prev = nullptr;
    delete timer;
    return;
  }
  // 尾节点
  if (timer == tail) {
    tail = tail->prev;
    tail->next = nullptr;
    delete timer;
    return;
  }
  // 中间节点
  timer->prev->next = timer->next;
  timer->next->prev = timer->prev;
  delete timer;
}

void sort_timer_lst::tick() {
  if (!head)
    return;
  locker_guard guard(lst_lock);
  time_t cur = time(nullptr);
  util_timer *tmp = head;
  while (tmp) {
    if (cur < tmp->expire)
      break;
    // 执行超时回调
    tmp->cb_func(tmp->user_data);
    // 移除头节点
    head = tmp->next;
    if (head)
      head->prev = nullptr;
    // 释放内存
    util_timer *del = tmp;
    tmp = head;
    delete del;
  }
}

void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head) {
  util_timer *prev = lst_head;
  util_timer *tmp = prev->next;
  while (tmp) {
    if (timer->expire < tmp->expire) {
      prev->next = timer;
      timer->next = tmp;
      tmp->prev = timer;
      timer->prev = prev;
      break;
    }
    prev = tmp;
    tmp = tmp->next;
  }
  // 插到尾部
  if (!tmp) {
    prev->next = timer;
    timer->prev = prev;
    timer->next = nullptr;
    tail = timer;
  }
}

// ===================== Utils 实现 =====================
Utils::Utils() : m_TIMESLOT(0) {}

Utils::~Utils() {
  // 关闭管道
  if (u_pipefd[0] > 0)
    close(u_pipefd[0]);
  if (u_pipefd[1] > 0)
    close(u_pipefd[1]);
  u_pipefd[0] = u_pipefd[1] = 0;
  u_epollfd = 0;
}

void Utils::init(int timeslot) {
  m_TIMESLOT = timeslot;
  // io_uring 下管道由 WebServer::eventListen 创建和管理，这里仅保存定时节拍
}

int Utils::setnonblocking(int fd) {
  int old_option = fcntl(fd, F_GETFL);
  int new_option = old_option | O_NONBLOCK;
  fcntl(fd, F_SETFL, new_option);
  return old_option;
}

void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode) {
  epoll_event event;
  event.data.fd = fd;

  if (1 == TRIGMode)
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
  else
    event.events = EPOLLIN | EPOLLRDHUP;

  if (one_shot)
    event.events |= EPOLLONESHOT;
  epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
  setnonblocking(fd);
}

void Utils::sig_handler(int sig) {
  int save_errno = errno;
  int msg = sig;
  send(u_pipefd[1], (char *)&msg, 1, MSG_NOSIGNAL);
  errno = save_errno;
}

void Utils::addsig(int sig, void(handler)(int), bool restart) {
  (void)sig;
  struct sigaction sa;
  memset(&sa, '\0', sizeof(sa));
  sa.sa_handler = handler;
  if (restart)
    sa.sa_flags |= SA_RESTART;
  // 阻塞所有信号，保证处理期间不被打断
  sigfillset(&sa.sa_mask);
  assert(sigaction(sig, &sa, nullptr) != -1);
}

void Utils::timer_handler() {
  m_timer_lst.tick();
  alarm(m_TIMESLOT);
}

// 向客户端返回错误提示，然后直接断开连接
void Utils::show_error(int connfd, const char *info) {
  // 1. 把错误字符串发送给客户端浏览器
  send(connfd, info, strlen(info), MSG_NOSIGNAL);
  // 2. 关闭客户端socket，断开TCP连接
  close(connfd);
}

// 静态只读Getter
int Utils::get_epollfd() { return u_epollfd; }

int Utils::get_pipefd(int idx) {
  if (idx < 0 || idx > 1)
    return -1;
  return u_pipefd[idx];
}
void cb_func(client_data *user_data) {
  // 1. 判空防护：如果传入的连接上下文是空，直接退出，防止野指针崩溃
  if (!user_data)
    return;

  // 2. 关闭 socket fd（io_uring 下无需 epoll_ctl DEL）
  close(user_data->sockfd);

  // 3. 全局在线连接计数-1，统计当前服务活跃客户端数量
  http_conn::m_user_count--;
}
