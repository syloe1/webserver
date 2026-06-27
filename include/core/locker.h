#ifndef LOCKER_H
#define LOCKER_H
#include <cerrno> //EAGAIN非阻塞返回判断
#include <exception>
#include <pthread.h>
#include <semaphore.h>
#include <stdexcept> //runtime_error 带error info

class sem {
public:
  // 无参构造：初始信号量计数=0
  sem() {
    // &m_sem：信号量变量地址
    // 第二个参数0：仅进程内多线程共享（不用进程间）
    // 第三个参数0：初始资源数量为0
    if (sem_init(&m_sem, 0, 0) != 0) {
      // 初始化失败抛异常，上层必须捕获，否则程序崩溃
      throw std::runtime_error("sem init failed");
    }
  }

  // 带参构造：自定义初始资源数量 num
  sem(int num) {
    if (sem_init(&m_sem, 0, num) != 0) {
      throw std::runtime_error("sem init failed");
    }
  }

  // 析构函数：对象销毁时自动释放信号量资源
  ~sem() { sem_destroy(&m_sem); }

  // P操作：申请资源
  bool wait() {
    // sem_wait成功返回0，转成布尔true；失败false
    return sem_wait(&m_sem) == 0;
  }
  // 非阻塞申请资源， 拿不到直接返回false
  bool try_wait() {
    int ret = sem_trywait(&m_sem);
    if (ret == 0)
      return true;
    if (errno == EAGAIN)
      return false;
    throw std::runtime_error("sem_trywait error");
  }
  // V操作：释放资源
  bool post() { return sem_post(&m_sem) == 0; }

  // 重新初始化信号量计数（销毁旧值后重建，用于连接池init）
  void reinit(int num) {
    sem_destroy(&m_sem);
    if (sem_init(&m_sem, 0, num) != 0)
      throw std::runtime_error("sem reinit failed");
  }

  // 限时等待：ms_timeout 毫秒超时返回false
  bool timewait(int ms_timeout) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms_timeout / 1000;
    ts.tv_nsec += (ms_timeout % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
      ts.tv_sec += 1;
      ts.tv_nsec -= 1000000000;
    }
    return sem_timedwait(&m_sem, &ts) == 0;
  }

private:
  // 底层原生信号量结构体，私有，外部无法直接操作
  sem_t m_sem;
  sem(const sem &) = delete;
  sem &operator=(const sem &) = delete;
};

class locker {
public:
  // 构造函数：初始化互斥锁
  locker() {
    // 第二个参数 nullptr：使用默认锁属性（普通互斥锁）
    if (pthread_mutex_init(&m_mutex, nullptr) != 0) {
      // 初始化失败抛异常，程序终止
      throw std::exception();
    }
  }

  // 析构：销毁锁，释放内核资源
  ~locker() { pthread_mutex_destroy(&m_mutex); }

  // 加锁，阻塞式
  bool lock() {
    // 成功返回true，失败false
    return pthread_mutex_lock(&m_mutex) == 0;
  }
  // 非阻塞加锁,抢不到锁直接false
  bool trylock() {
    int ret = pthread_mutex_trylock(&m_mutex);
    if (ret == 0)
      return true;
    if (ret == EBUSY)
      return false;
    throw std::runtime_error("pthread_mutex_trylock error");
  }
  // 解锁
  bool unlock() { return pthread_mutex_unlock(&m_mutex) == 0; }

  // 获取底层 pthread_mutex_t 指针
  pthread_mutex_t *get() { return &m_mutex; }

private:
  // 底层原生互斥锁，私有隔离
  pthread_mutex_t m_mutex;
  locker(const locker &) = delete;
  locker &operator=(const locker &) = delete;
};
class cond {
public:
  // 构造：初始化条件变量
  cond() {
    if (pthread_cond_init(&m_cond, nullptr) != 0) {
      throw std::exception();
    }
  }

  // 析构：释放内核条件变量资源
  ~cond() { pthread_cond_destroy(&m_cond); }

  // 阻塞等待，必须传入一把已经上锁的mutex
  bool wait(pthread_mutex_t *m_mutex) {
    int ret = 0;
    ret = pthread_cond_wait(&m_cond, m_mutex);
    return ret == 0;
  }

  // 限时等待，超时自动退出阻塞
  bool timewait(pthread_mutex_t *m_mutex, struct timespec t) {
    int ret = 0;
    ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
    return ret == 0;
  }

  // 唤醒一个正在等待的线程
  bool signal() { return pthread_cond_signal(&m_cond) == 0; }

  // 唤醒所有正在等待的线程
  bool broadcast() { return pthread_cond_broadcast(&m_cond) == 0; }

private:
  // 底层原生条件变量，私有封装隔离
  pthread_cond_t m_cond;
  cond(const cond &) = delete;
  cond &operator=(const cond &) = delete;
};
class locker_guard {
public:
  explicit locker_guard(locker &lk) : m_lk(lk) { m_lk.lock(); }
  ~locker_guard() { m_lk.unlock(); }
  locker_guard(const locker_guard &) = delete;
  locker_guard &operator=(const locker_guard &) = delete;

private:
  locker &m_lk;
};
#endif
