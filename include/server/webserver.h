#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <arpa/inet.h>
#include <cassert>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "core/buffer_ring.h"
#include "core/io_uring_engine.h"
#include "core/locker.h"
#include "core/lst_timer.h"
#include "core/threadpool.h"
#include "net/http_conn.h"

// 全局默认常量，可通过init参数动态覆盖
const int DEFAULT_MAX_FD = 65536;
const int DEFAULT_TIMESLOT = 5;

class WebServer {
public:
  WebServer();
  ~WebServer();

  // 初始化，支持动态传入超时、最大fd、事件上限，覆盖默认常量
  void init(int port, std::string user, std::string passWord,
            std::string databaseName, int log_write, int opt_linger,
            int trigmode, int sql_num, int thread_num, int close_log,
            int actor_model, int timeslot = DEFAULT_TIMESLOT,
            int max_fd = DEFAULT_MAX_FD,
            int max_event = 10000);

  void thread_pool();
  void sql_pool();
  void log_write();
  void trig_mode();
  void eventListen();
  void eventLoop();

  // 定时器相关
  void timer(int connfd, struct sockaddr_in client_address);
  void adjust_timer(util_timer *timer);
  void deal_timer(util_timer *timer, int sockfd);

  // 事件分发处理（io_uring 版本）
  bool dealclientdata();
  bool dealwithsignal(bool &timeout, bool &stop_server);

  // ========== 只读Getter接口，外部仅能读取，不可修改 ==========
  int get_port() const;
  const char *get_root() const;
  int get_log_write() const;
  int get_close_log() const;
  int get_actor_model() const;
  int get_listenfd() const;
  int get_opt_linger() const;
  int get_sql_num() const;
  int get_thread_num() const;
  int get_time_slot() const;
  int get_max_fd() const;
  int get_max_event() const;

  std::string get_db_user() const;
  std::string get_db_passwd() const;
  std::string get_db_name() const;

  // 设置网站根目录（仅初始化阶段调用，运行时禁止修改）
  void set_root(const char *root);

  // 禁止拷贝构造、赋值，持有 io_uring/管道/线程池等不可复制资源
  WebServer(const WebServer &) = delete;
  WebServer &operator=(const WebServer &) = delete;

private:
  // ========== 全部成员私有，外部无法直接访问 ==========
  // 基础运行配置
  int m_port;
  char *m_root;
  int m_log_write;
  int m_close_log;
  int m_actormodel;

  // 信号管道（保持 pipe，用 io_uring 异步读）
  int m_pipefd[2];
  char m_signal_buf[1024];

  // io_uring 引擎 + BufferPool（替代 epoll）
  IoUringEngine m_uring;
  BufferPool   *m_buf_pool = nullptr;

  // 动态可配置上限，替代全局硬编码常量
  int m_MAX_FD;
  int m_MAX_EVENT_NUMBER;
  int m_TIMESLOT;

  // 客户端HTTP连接数组
  http_conn *users;
  // 定时器客户端上下文数组
  client_data *users_timer;

  // 数据库连接池
  connection_pool *m_connPool;
  std::string m_user;
  std::string m_passWord;
  std::string m_databaseName;
  int m_sql_num;

  // 业务线程池
  threadpool<http_conn> *m_pool;
  int m_thread_num;

  // 监听套接字、TCP linger 配置
  int m_listenfd;
  int m_OPT_LINGER;

  // 定时器工具类
  Utils utils;

  // 新增：多线程共享资源互斥锁，保护定时器链表、连接数组
  locker m_conn_lock;
  locker m_timer_lock;
};

#endif
