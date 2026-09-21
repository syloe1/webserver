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
  // 创建MySQL连接池，并把用户表加载进内存缓存
  server.sql_pool();
  // 创建阻塞卸载池（协程版：MySQL 这类阻塞调用都丢给它）
  server.thread_pool();
  // 空实现，保留只为兼容既有启动流程
  server.trig_mode();
  // 创建 N 个 Scheduler（每载体线程一个 io_uring + SO_REUSEPORT 监听 socket）
  server.eventListen();
  // 启动载体线程并 join 到收到 SIGINT/SIGTERM
  server.eventLoop();

  return 0;
}
