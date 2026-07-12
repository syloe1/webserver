#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "core/buffer_ring.h"
#include "core/io_uring_engine.h"
#include "core/locker.h"
#include "core/log.h"
#include "core/lst_timer.h"
#include "db/sql_connection_pool.h"
#include <list>

class http_conn {
public:
  // 静态常量
  static const int FILENAME_LEN = 200;
  static const int READ_BUFFER_SIZE = 2048;
  static const int WRITE_BUFFER_SIZE = 1024;

  // HTTP请求方法
  enum METHOD {
    GET = 0,
    POST,
    HEAD,
    PUT,
    DELETE,
    TRACE,
    OPTIONS,
    CONNECT,
    PATH
  };
  // HTTP报文解析阶段状态机
  enum CHECK_STATE {
    CHECK_STATE_REQUESTLINE = 0,
    CHECK_STATE_HEADER,
    CHECK_STATE_CONTENT
  };
  // HTTP处理结果码
  enum HTTP_CODE {
    NO_REQUEST,
    GET_REQUEST,
    BAD_REQUEST,
    NO_RESOURCE,
    FORBIDDEN_REQUEST,
    FILE_REQUEST,
    INTERNAL_ERROR,
    REDIRECT,
    CLOSED_CONNECTION
  };
  // 单行解析状态
  enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };

public:
  http_conn();
  ~http_conn();

  // 禁止拷贝，持有fd、数据库、mmap资源不可复制
  http_conn(const http_conn &) = delete;
  http_conn &operator=(const http_conn &) = delete;

  // 对外初始化接口，全部使用std::string
  void init(int sockfd, const sockaddr_in &addr, std::string root,
            int trig_mode, int close_log, std::string sql_user,
            std::string sql_passwd, std::string sql_db);

  // 关闭连接，释放mmap、归还数据库连接
  void close_conn(bool real_close = true);

  // 业务主逻辑：解析请求 + 组装响应
  void process();

  // 非阻塞读取TCP数据（首次 accept 后仍可用）
  bool read_once();

  // 获取客户端地址
  sockaddr_in *get_address() { return &m_address; }

  // 预加载数据库用户表
  void initmysql_result(connection_pool *connPool);

  // ==== io_uring 异步 I/O 接口 ====
  void submit_recv();                        // 主线程直接提交 RECV
  void on_recv_done(int bytes_read, int buf_id);  // RECV CQE 回调
  void on_send_done();                       // SEND CQE 回调（主线程）
  bool on_send_cqe(int bytes_sent);           // 处理 SEND CQE，返回 true=完成 false=需重传

  // 定时器标记、线程同步标记
  int timer_flag;
  int improv;

  // ========== 只读Getter，调试可读，外部不可修改 ==========
  int get_sockfd() const;
  int get_state() const;
  std::string get_url() const;
  std::string get_doc_root() const;
  long get_content_length() const;
  bool is_linger() const;
  METHOD get_method() const;

public:
  // 全局 io_uring 引擎 + BufferPool + 单线程统一提交队列
  static IoUringEngine *m_ring;
  static BufferPool   *s_buf_pool;
  static int m_user_count;
  static locker m_count_lock;

  // worker → main 线程的待提交队列（锁 + list）
  static locker s_sq_lock;
  static std::list<http_conn *> s_sq_queue;
  static int    s_wakeup_fd;  // pipe 写端 fd，worker 入队后写 1 字节唤醒主线程
  static void enqueue_to_main(http_conn *conn);
  static void flush_main_queue();  // 主线程 eventLoop 调用

  MYSQL *mysql;
  int m_state;

  // worker 线程设标记，主线程读并清空
  bool m_need_send = false;
  bool m_need_recv = false;

private:
  void init();

  // 完整解析HTTP请求报文
  HTTP_CODE process_read();

  // 根据解析结果组装响应报文
  bool process_write(HTTP_CODE ret);

  // 分段解析HTTP
  HTTP_CODE parse_request_line(const char *text);
  HTTP_CODE parse_headers(const char *text);
  HTTP_CODE parse_content(const char *text);

  // 路由业务：读取静态文件 / CGI数据库登录校验
  HTTP_CODE do_request();

  // 获取当前解析行起始指针
  char *get_line() { return &m_read_buf[m_start_line]; };

  // 按\r\n截取单行，返回行状态
  LINE_STATUS parse_line();

  // 释放mmap文件映射内存
  void unmap();

  // 响应拼接工具函数
  bool add_response(const char *format, ...);
  bool add_content(const char *content);
  bool add_status_line(int status, const char *title);
  bool add_headers(int content_length);
  bool add_content_type();
  bool add_content_length(int content_length);
  bool add_linger();
  bool add_blank_line();

private:
  // TCP套接字
  int m_sockfd;
  sockaddr_in m_address;

  // 原固定数组保留基础缓冲区，业务存储改用string，兼顾ET分段读取
  char m_read_buf[READ_BUFFER_SIZE];
  long m_read_idx;
  long m_checked_idx;
  int m_start_line;

  // 响应缓冲区
  char m_write_buf[WRITE_BUFFER_SIZE];
  int m_write_idx;

  // HTTP解析状态
  CHECK_STATE m_check_state;
  METHOD m_method;

  // 文件路径，替换定长数组为string
  std::string m_real_file;
  std::string m_url;
  std::string m_version;
  std::string m_host;
  long m_content_length;
  bool m_linger;

  // 文件mmap映射
  char *m_file_address;
  struct stat m_file_stat;
  // 分散写iovec：响应头 + 文件内容
  struct iovec m_iv[2];
  int m_iv_count;

  // POST CGI标记，请求体缓存改用string
  int cgi;
  std::string m_post_data;

  // 发送进度标记
  int bytes_to_send;
  int bytes_have_send;

  // 网站静态资源根目录
  std::string doc_root;

  // 运行配置
  int m_TRIGMode;
  int m_close_log;

  // 数据库账号库名，替换定长char数组
  std::string sql_user;
  std::string sql_passwd;
  std::string sql_name;
};

#endif
