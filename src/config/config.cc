#include "config/config.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// 构造函数：给所有配置项设置程序默认启动参数
Config::Config() {
  PORT = 9006;        // Web服务监听端口，默认9006
  LOGWrite = 0;       // 日志模式：0同步日志、1异步阻塞队列日志，默认同步
  TRIGMode = 0;       // epoll触发模式组合总开关：0 LT水平触发、1 ET边缘触发
  LISTENTrigmode = 0; // listenfd监听套接字触发方式：0 LT、1 ET
  CONNTrigmode = 0;   // 客户端连接connfd读写触发方式：0 LT、1 ET
  OPT_LINGER =
      0; // TCP优雅关闭开关：0关闭、1开启，开启后等待缓冲区数据传输完成再关闭
  sql_num = 8;     // MySQL数据库连接池最大连接数量，默认8条
  thread_num = 8;  // 业务线程池工作线程数量，默认8个
  close_log = 0;   // 全局日志总开关：0开启日志、1关闭日志
  actor_model = 0; // 并发IO模型：0 Proactor模型、1 Reactor模型，默认Proactor
}

// 打印程序启动参数帮助说明，输入错误参数时调用
void Config::print_usage() const {
  printf("Usage: ./webserver [OPTIONS]\n");
  printf("Options:\n");
  printf("  -p num   Listen port (1-65535, default 9006)\n");
  printf("  -l 0/1   Log mode:0 sync,1 async (default 0)\n");
  printf("  -m 0-3   Epoll trigger combo (default 0 LT+LT)\n");
  printf("  -o 0/1   Enable linger close (default 0 off)\n");
  printf("  -s num   Mysql connection pool size (>=1, default 8)\n");
  printf("  -t num   Thread pool worker count (>=1, default 8)\n");
  printf("  -c 0/1   Close log:1 disable,0 enable (default 0)\n");
  printf("  -a 0/1   Actor model:0 Proactor,1 Reactor (default 0)\n");
}

// 解析命令行启动参数，覆盖默认配置
void Config::parse_arg(int argc, char *argv[]) {
  int opt;
  // 可解析参数列表，带冒号代表该参数后必须附带数值
  const char *opt_str = "p:l:m:o:s:t:c:a:";
  while ((opt = getopt(argc, argv, opt_str)) != -1) {
    switch (opt) {
    case 'p':
      PORT = atoi(optarg); // 覆盖监听端口
      break;
    case 'l':
      LOGWrite = atoi(optarg); // 覆盖日志读写模式
      break;
    case 'm':
      TRIGMode = atoi(optarg); // 覆盖epoll触发模式
      break;
    case 'o':
      OPT_LINGER = atoi(optarg); // 覆盖TCP优雅关闭开关
      break;
    case 's':
      sql_num = atoi(optarg); // 覆盖数据库连接池连接数
      break;
    case 't':
      thread_num = atoi(optarg); // 覆盖线程池工作线程数量
      break;
    case 'c':
      close_log = atoi(optarg); // 覆盖全局日志开关
      break;
    case 'a':
      actor_model = atoi(optarg); // 覆盖并发IO模型
      break;
    default:
      // 未知参数，打印帮助并退出程序
      print_usage();
      exit(EXIT_FAILURE);
    }
  }
}

// 校验所有配置参数合法性，非法参数返回false，程序禁止启动
bool Config::check_valid() const {
  // 端口合法范围 1~65535，0和超过65535均非法
  if (PORT <= 0 || PORT > 65535)
    return false;
  // 日志模式仅允许0同步、1异步
  if (LOGWrite < 0 || LOGWrite > 1)
    return false;
  // epoll触发模式：0 LT+LT, 1 LT+ET, 2 ET+LT, 3 ET+ET
  if (TRIGMode < 0 || TRIGMode > 3)
    return false;
  // TCP优雅关闭仅允许0关闭、1开启
  if (OPT_LINGER < 0 || OPT_LINGER > 1)
    return false;
  // 数据库连接池、线程池数量必须大于0，不能为0或负数
  if (sql_num <= 0 || thread_num <= 0)
    return false;
  // 日志开关仅允许0开启、1关闭
  if (close_log < 0 || close_log > 1)
    return false;
  // 并发模型仅允许0 Proactor、1 Reactor
  if (actor_model < 0 || actor_model > 1)
    return false;
  return true;
}

// ---------------- 只读Getter接口实现，外部只能读取配置，禁止修改
// ---------------- 获取监听端口
int Config::get_port() const { return PORT; }
// 获取日志读写模式
int Config::get_log_write() const { return LOGWrite; }
// 获取epoll触发模式总开关
int Config::get_trig_mode() const { return TRIGMode; }
// 获取listenfd监听fd触发模式
int Config::get_listen_trig() const { return LISTENTrigmode; }
// 获取客户端connfd连接fd触发模式
int Config::get_conn_trig() const { return CONNTrigmode; }
// 获取TCP优雅关闭开关
int Config::get_opt_linger() const { return OPT_LINGER; }
// 获取MySQL连接池最大连接数
int Config::get_sql_num() const { return sql_num; }
// 获取线程池工作线程数量
int Config::get_thread_num() const { return thread_num; }
// 获取全局日志关闭开关
int Config::get_close_log() const { return close_log; }
// 获取并发IO模型标识
int Config::get_actor_model() const { return actor_model; }
