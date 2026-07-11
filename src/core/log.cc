#include "core/log.h"
#include "core/locker.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdexcept>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
using namespace std;

Log::Log()
    : m_close_log(0), m_split_lines(0), m_log_buf_size(0), m_count(0),
      m_today(0), m_fp(nullptr), m_buf(nullptr), m_log_queue(nullptr),
      m_is_async(false) {}

Log::~Log() {
  // 1. 异步模式：唤醒阻塞的写线程，等待队列消费完毕
  if (m_is_async && m_log_queue != nullptr) {
    // 往队列塞一条空日志唤醒线程，循环pop结束
    string empty_msg;
    m_log_queue->push(empty_msg);
    // 简易等待，实际项目可用pthread_join保存tid等待回收
    usleep(100000);
    delete m_log_queue;
    m_log_queue = nullptr;
  }

  // 2. 释放格式化缓冲区
  if (m_buf != nullptr) {
    delete[] m_buf;
    m_buf = nullptr;
  }

  // 3. 关闭日志文件
  if (m_fp != nullptr) {
    locker_guard guard(m_mutex);
    fflush(m_fp);
    fclose(m_fp);
    m_fp = nullptr;
  }
}

bool Log::init(const char *file_name, int close_log, int log_buf_size,
               int split_lines, int max_queue_size) {
  m_close_log = close_log;
  m_log_buf_size = log_buf_size;
  m_split_lines = split_lines;

  // 分配格式化缓冲区
  if (m_log_buf_size <= 0)
    throw runtime_error("log buf size invalid");
  m_buf = new char[m_log_buf_size];
  memset(m_buf, '\0', m_log_buf_size);

  // 开启异步日志，创建阻塞队列+后台写线程
  if (max_queue_size >= 1) {
    m_is_async = true;
    m_log_queue = new block_queue<string>(max_queue_size);
    pthread_t tid;
    int ret = pthread_create(&tid, nullptr, flush_log_thread, nullptr);
    if (ret != 0)
      throw runtime_error("create log flush thread failed");
  }

  // 拆分路径与文件名
  time_t t = time(NULL);
  struct tm *sys_tm = localtime(&t);
  struct tm my_tm = *sys_tm;
  const char *p = strrchr(file_name, '/');
  char log_full_name[384] = {0};

  if (p == nullptr) {
    snprintf(log_full_name, sizeof(log_full_name), "%d_%02d_%02d_%s",
             my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
  } else {
    strncpy(log_name, p + 1, sizeof(log_name) - 1);
    strncpy(dir_name, file_name, p - file_name + 1);
    snprintf(log_full_name, sizeof(log_full_name), "%s%d_%02d_%02d_%s",
             dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
             log_name);
  }

  m_today = my_tm.tm_mday;
  m_fp = fopen(log_full_name, "a");
  if (m_fp == nullptr)
    return false;
  return true;
}

void Log::write_log(int level, const char *format, ...) {
  // 防御: 日志系统未初始化或 m_fp 为空时直接跳过，避免 fclose(NULL) 崩溃
  if (m_fp == nullptr) {
    return;
  }

  struct timeval now = {0, 0};
  gettimeofday(&now, nullptr);
  time_t t = now.tv_sec;
  struct tm *sys_tm = localtime(&t);
  struct tm my_tm = *sys_tm;
  char s[16] = {0};

  switch (level) {
  case 0:
    strcpy(s, "[debug]:");
    break;
  case 1:
    strcpy(s, "[info]:");
    break;
  case 2:
    strcpy(s, "[warn]:");
    break;
  case 3:
    strcpy(s, "[erro]:");
    break;
  default:
    strcpy(s, "[info]:");
    break;
  }

  va_list valst;
  va_start(valst, format);
  string log_str;

  // 合并锁区间：计数更新、切割判断、格式化缓冲区统一一把锁
  locker_guard guard(m_mutex);
  m_count++;

  // 判断是否需要切割日志（m_split_lines 必须 > 0 才做切割，防止除零）
  if (m_split_lines > 0 &&
      (m_today != my_tm.tm_mday || m_count % m_split_lines == 0)) {
    char new_log[384] = {0};
    fflush(m_fp);
    fclose(m_fp);
    char tail[32] = {0};
    snprintf(tail, sizeof(tail), "%d_%02d_%02d_", my_tm.tm_year + 1900,
             my_tm.tm_mon + 1, my_tm.tm_mday);

    if (m_today != my_tm.tm_mday) {
      snprintf(new_log, sizeof(new_log), "%s%s%s", dir_name, tail,
               log_name);
      m_today = my_tm.tm_mday;
      m_count = 0;
    } else {
      snprintf(new_log, sizeof(new_log), "%s%s%s.%lld", dir_name, tail,
               log_name, m_count / m_split_lines);
    }
    m_fp = fopen(new_log, "a");
  }

  // 格式化日志，校验返回值防止缓冲区溢出
  int n = snprintf(m_buf, m_log_buf_size - 2,
                   "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                   my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                   my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
  if (n < 0 || n >= m_log_buf_size - 2)
    n = m_log_buf_size - 3;

  int m = vsnprintf(m_buf + n, m_log_buf_size - n - 2, format, valst);
  if (m < 0)
    m = 0;

  m_buf[n + m] = '\n';
  m_buf[n + m + 1] = '\0';
  log_str = m_buf;

  va_end(valst);

  // 异步队列未满则丢队列，否则同步落盘
  if (m_is_async && !m_log_queue->full()) {
    m_log_queue->push(log_str);
  } else {
    fputs(log_str.c_str(), m_fp);
    fflush(m_fp);  // 立即落盘，否则 libc 缓冲导致日志丢失
  }
}

void Log::flush(void) {
  locker_guard guard(m_mutex);
  if (m_fp != nullptr) {
    fflush(m_fp);
  }
}

// 后台消费线程：阻塞读取日志并写入文件
void *Log::async_write_log() {
  string single_log;
  while (m_log_queue->pop(single_log)) {
    if (single_log.empty())
      break; // 空消息作为退出标记
    locker_guard guard(m_mutex);
    fputs(single_log.c_str(), m_fp);
    fflush(m_fp);
  }
  return nullptr;
}
