#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H
#include "core/locker.h"
#include <iostream>
#include <pthread.h>
#include <stdlib.h>
#include <sys/time.h>
using namespace std;

template <class T> class block_queue {
public:
  block_queue(int max_size = 1000) {
    if (max_size <= 0) {
      throw runtime_error("block_queue max_size must > 0");
    }
    m_max_size = max_size;
    m_array = new T[max_size];
    m_size = 0;
    m_front = -1;
    m_back = -1;
  }
  void clear() {
    locker_guard guard(m_mutex);
    m_size = 0;
    m_front = -1;
    m_back = -1;
  }
  ~block_queue() {
    locker_guard guard(m_mutex);
    if (m_array != nullptr) {
      delete[] m_array;
      m_array = nullptr;
    }
  }
  bool full() {
    locker_guard guard(m_mutex);
    return m_size >= m_max_size;
  }
  bool empty() {
    locker_guard guard(m_mutex);
    return m_size == 0;
  }
  bool front(T &value) {
    locker_guard guard(m_mutex);
    if (m_size == 0)
      return false;
    value = m_array[m_front];
    return true;
  }
  bool back(T &value) {
    locker_guard guard(m_mutex);
    if (m_size == 0)
      return false;
    value = m_array[m_back];
    return true;
  }
  int size() {
    locker_guard guard(m_mutex);
    return m_size;
  }
  int max_size() {
    locker_guard guard(m_mutex);
    return m_max_size;
  }
  // 非阻塞入队， 队列满直接返回false
  bool push(const T &item) {
    locker_guard guard(m_mutex);
    if (m_size >= m_max_size) {
      return false;
    }
    m_back = (m_back + 1) % m_max_size;
    m_array[m_back] = item;
    m_size++;

    m_cond.signal();
    return true;
  }
  // 阻塞入队， 队列满时生产者阻塞等待
  void push_block(const T &item) {
    locker_guard guard(m_mutex);
    while (m_size >= m_max_size) {
      m_cond.wait(m_mutex.get());
    }
    m_back = (m_back + 1) % m_max_size;
    m_array[m_back] = item;
    m_size++;
    m_cond.signal();
  }
  // 阻塞出队， 无数据永久等待
  bool pop(T &item) {
    locker_guard guard(m_mutex);
    while (m_size <= 0) {
      if (!m_cond.wait(m_mutex.get())) {
        return false;
      }
    }
    m_front = (m_front + 1) % m_max_size;
    item = m_array[m_front];
    m_size--;
    m_cond.signal();
    return true;
  }
  // 带超时阻塞出队，ms_timeout毫秒超时返回false
  bool pop(T &item, int ms_timeout) {
    struct timespec t{};
    struct timeval now{};
    gettimeofday(&now, nullptr);

    locker_guard guard(m_mutex);
    // 队列为空则限时等待
    while (m_size <= 0) {
      t.tv_sec = now.tv_sec + ms_timeout / 1000;
      // 修正纳秒计算：剩余毫秒转纳秒
      t.tv_nsec = (ms_timeout % 1000) * 1000000;
      if (!m_cond.timewait(m_mutex.get(), t)) {
        // 超时直接返回，守卫自动解锁
        return false;
      }
    }
    // 唤醒后再次判断，防止虚假唤醒
    if (m_size <= 0) {
      return false;
    }
    m_front = (m_front + 1) % m_max_size;
    item = m_array[m_front];
    m_size--;
    m_cond.signal();
    return true;
  }

private:
  block_queue(const block_queue &) = delete;
  block_queue &operator=(const block_queue &) = delete;
  locker m_mutex;
  cond m_cond;

  T *m_array;
  int m_size;
  int m_max_size;
  int m_front;
  int m_back;
};
#endif
