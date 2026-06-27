#include "config/config.h"
#include <cstdio>
#include <cstring>
#include <string>
using namespace std;

// 数据库配置抽离，方便统一修改，后续可迁移到配置文件
const string DB_USER = "root";
const string DB_PASSWD = "qaz123";
const string DB_NAME = "db";

int main(int argc, char *argv[]) {
  // 1. 初始化配置类并解析命令行参数
  Config config;
  config.parse_arg(argc, argv);

  // 2. 校验配置合法性，非法则打印帮助并退出
  if (!config.check_valid()) {
    config.print_usage();
    return EXIT_FAILURE;
  }

  WebServer server;

  // 3. 全部使用只读getter接口，不再直接访问私有成员
  server.init(config.get_port(), DB_USER, DB_PASSWD, DB_NAME,
              config.get_log_write(), config.get_opt_linger(),
              config.get_trig_mode(), config.get_sql_num(),
              config.get_thread_num(), config.get_close_log(),
              config.get_actor_model());

  // 初始化日志模块
  server.log_write();
  // 创建MySQL连接池
  server.sql_pool();
  // 创建业务线程池
  server.thread_pool();
  // 设置epoll触发模式 LT/ET
  server.trig_mode();
  // 创建监听socket并注册epoll
  server.eventListen();
  // 主线程epoll事件循环，服务正式运行
  server.eventLoop();

  return 0;
}
