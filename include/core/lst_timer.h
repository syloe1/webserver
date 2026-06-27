#ifndef LST_TIMER
#define LST_TIMER

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/locker.h" // 引入互斥锁
#include "core/log.h"
#include <time.h>

class util_timer;

// 客户端连接上下文：socket、地址、绑定定时器
struct client_data {
  sockaddr_in address; // 客户端IP+端口
  int sockfd;          // 客户端连接fd
  util_timer *timer;   // 关联的定时器节点
};

// 定时器双向链表节点
class util_timer {
public:
  time_t expire;                  // 到期绝对时间戳
  void (*cb_func)(client_data *); // 超时回调函数指针
  client_data *user_data;         // 绑定客户端上下文
  util_timer *prev, *next;        // 双向链表前后指针

  util_timer()
      : prev(nullptr), next(nullptr), expire(0), cb_func(nullptr),
        user_data(nullptr) {}
};

// 有序升序双向定时器链表
class sort_timer_lst {
public:
  sort_timer_lst();
  ~sort_timer_lst();

  // 添加定时器节点
  void add_timer(util_timer *timer);
  // 刷新定时器超时时间，调整节点位置
  void adjust_timer(util_timer *timer);
  // 删除指定定时器节点
  void del_timer(util_timer *timer);
  // 定时触发，清理所有已到期定时器
  void tick();

private:
  // 递归插入节点内部重载
  void add_timer(util_timer *timer, util_timer *lst_head);

  util_timer *head;
  util_timer *tail;
  locker lst_lock; // 新增：互斥锁保护链表，多线程增删安全

  // 禁止拷贝、赋值（持有链表资源，不可复制）
  sort_timer_lst(const sort_timer_lst &) = delete;
  sort_timer_lst &operator=(const sort_timer_lst &) = delete;
};

// 全局工具类：epoll、信号、定时器、fd工具统一封装
class Utils {
public:
  Utils();
  ~Utils();

  // 初始化定时节拍、管道、信号
  void init(int timeslot);

  // 设置fd为非阻塞IO
  int setnonblocking(int fd);

  // epoll注册fd读事件，支持ET/LT、EPOLLONESHOT
  void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

  // 信号统一处理静态回调
  static void sig_handler(int sig);

  // 注册自定义信号处理函数，支持SA_RESTART
  void addsig(int sig, void(handler)(int), bool restart = true);

  // SIGALRM触发的定时任务，驱动定时器tick
  void timer_handler();

  // 向客户端输出错误信息
  void show_error(int connfd, const char *info);

  // 静态只读获取epollfd，禁止外部直接修改
  static int get_epollfd();
  // 静态只读获取管道fd数组，禁止外部直接修改
  static int get_pipefd(int idx);

public:
  // 全局epoll fd与管道，WebServer主控直接设置
  static int u_pipefd[2];
  static int u_epollfd;

public:
  sort_timer_lst m_timer_lst; // 有序定时器链表
  int m_TIMESLOT;             // 定时节拍（秒）

  // 禁止拷贝、赋值（持有管道、epoll、定时器资源）
  Utils(const Utils &) = delete;
  Utils &operator=(const Utils &) = delete;
};

// 全局默认超时回调：关闭闲置超时连接
void cb_func(client_data *user_data);

#endif
