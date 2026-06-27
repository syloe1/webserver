#ifndef LOG_H
#define LOG_H
#include "core/block_queue.h"
#include <iostream>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
using namespace std;
class Log {
public:
  static Log *get_instance() {
    static Log instance;
    return &instance;
  }
  static void *flush_log_thread(void *args) {
    (void)args;
    Log::get_instance()->async_write_log();
    return nullptr;
  }
  bool init(const char *file_name, int close_log, int log_buf_size = 8192,
            int split_line = 5000000, int max_queue_size = 0);
  void write_log(int level, const char *format, ...);

  void flush(void);

  // 宏访问成员，必须公开
  int m_close_log; // 关闭日志

private:
  Log();
  virtual ~Log();
  void *async_write_log();

private:
  char dir_name[128]; // 路径名
  char log_name[128]; // log文件名
  int m_split_lines;  // 日志最大行数
  int m_log_buf_size; // 日志缓冲区大小
  long long m_count;  // 日志行数记录
  int m_today;        // 因为按天分类,记录当前时间是那一天
  FILE *m_fp;         // 打开log的文件指针
  char *m_buf;
  block_queue<string> *m_log_queue; // 阻塞队列
  bool m_is_async;                  // 是否同步标志位
  locker m_mutex;
};

// 原裸if替换为 do{}while(0)，消除悬挂else
#define LOG_DEBUG(format, ...)                                                 \
  do {                                                                         \
    if (0 == Log::get_instance()->m_close_log) {                               \
      Log::get_instance()->write_log(0, format, ##__VA_ARGS__);                \
    }                                                                          \
  } while (0)

#define LOG_INFO(format, ...)                                                  \
  do {                                                                         \
    if (0 == Log::get_instance()->m_close_log) {                               \
      Log::get_instance()->write_log(1, format, ##__VA_ARGS__);                \
    }                                                                          \
  } while (0)

#define LOG_WARN(format, ...)                                                  \
  do {                                                                         \
    if (0 == Log::get_instance()->m_close_log) {                               \
      Log::get_instance()->write_log(2, format, ##__VA_ARGS__);                \
    }                                                                          \
  } while (0)

#define LOG_ERROR(format, ...)                                                 \
  do {                                                                         \
    if (0 == Log::get_instance()->m_close_log) {                               \
      Log::get_instance()->write_log(3, format, ##__VA_ARGS__);                \
    }                                                                          \
  } while (0)

#endif
