#ifndef CONFIG_H
#define CONFIG_H
// 全局命令行配置解析类
// 作用：统一管理Web服务所有运行参数，解析启动命令行参数，提供只读配置，保证参数运行期间不可篡改
#include "server/webserver.h"
using namespace std;

class Config {
public:
  // 构造函数：初始化所有配置项默认值，不传入启动参数时使用这套默认配置运行服务
  Config();
  // 虚析构，预留后续子类继承扩展的能力，无资源释放需求使用default默认实现
  virtual ~Config() = default;

  // 解析命令行启动参数，使用getopt解析 -p/-t/-s
  // 等短参数，覆盖构造函数的默认配置
  void parse_arg(int argc, char *argv[]);
  // 打印程序启动参数帮助文档，输入非法/未知参数时调用，提示用户正确启动命令
  void print_usage() const;
  // 校验所有配置数值是否合法，端口、线程数、连接池数量、开关参数校验；非法返回false，服务禁止启动
  bool check_valid() const;

  // ========== Getter 只读接口 ==========
  // 获取Web服务监听端口
  int get_port() const;
  // 获取日志写入模式：0同步日志、1异步阻塞队列日志
  int get_log_write() const;
  // epoll触发模式总控制标识：0 LT水平触发、1 ET边缘触发
  int get_trig_mode() const;
  // listenfd监听套接字的epoll触发模式
  int get_listen_trig() const;
  // 客户端连接connfd读写事件的epoll触发模式
  int get_conn_trig() const;
  // TCP优雅关闭开关：0关闭、1开启，开启后等待缓冲区数据传输完成再释放连接
  int get_opt_linger() const;
  // MySQL数据库连接池最大连接数量
  int get_sql_num() const;
  // 业务线程池工作线程数量，控制并发HTTP请求处理能力
  int get_thread_num() const;
  // 全局日志总开关：0开启日志输出、1关闭所有日志，提升线上性能
  int get_close_log() const;
  // 并发IO模型标识：0 Proactor模型、1 Reactor模型
  int get_actor_model() const;

private:
  // 全部配置成员私有，外部无法直接修改，杜绝运行中配置被篡改引发逻辑错乱
  int PORT;           // Web监听端口
  int LOGWrite;       // 同步/异步日志标识
  int TRIGMode;       // epoll LT/ET总开关
  int LISTENTrigmode; // 监听fd触发模式
  int CONNTrigmode;   // 客户端连接fd触发模式
  int OPT_LINGER;     // TCP优雅关闭开关
  int sql_num;        // 数据库连接池连接数
  int thread_num;     // 线程池工作线程数
  int close_log;      // 日志关闭总开关
  int actor_model;    // Proactor/Reactor并发模型

  // 禁止拷贝构造、赋值运算符重载
  // 配置为全局唯一资源，拷贝会生成两份独立配置，参数不一致导致服务逻辑异常，编译期拦截拷贝代码
  Config(const Config &) = delete;
  Config &operator=(const Config &) = delete;
};

#endif
