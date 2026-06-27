# TinyWebServer

高性能 C++11 多线程 Web 服务器，基于 **Epoll ET/LT + Reactor/Proactor + 线程池 + MySQL 连接池 + 定时器** 实现。

## 项目结构

```
TinyWebServer/
├── CMakeLists.txt              # 根编译脚本
├── main.cc                     # 程序入口
├── include/                    # 所有头文件
│   ├── config/config.h         # 命令行配置解析
│   ├── core/                   # 核心组件
│   │   ├── locker.h            # 互斥锁/信号量/条件变量/RAII守卫
│   │   ├── log.h               # 同步/异步日志
│   │   ├── block_queue.h       # 线程安全阻塞队列
│   │   ├── lst_timer.h         # 升序双向定时器链表 + Utils工具类
│   │   └── threadpool.h        # 工作线程池模板
│   ├── db/                     # 数据库模块
│   │   ├── sql_connection_pool.h   # MySQL连接池(单例)
│   │   └── user_cache.h            # 全局用户缓存(单例)
│   ├── net/                    # 网络层
│   │   ├── socket_tool.h       # epoll/socket 工具函数
│   │   ├── http_const.h        # HTTP响应状态码/错误页面模板
│   │   └── http_conn.h         # HTTP连接类(解析+路由+响应)
│   └── server/webserver.h      # WebServer顶层总控
├── src/                        # 所有实现文件(与include对应)
│   ├── config/config.cc
│   ├── core/  (log.cc, lst_timer.cc)
│   ├── db/    (sql_connection_pool.cc, user_cache.cc)
│   ├── net/   (socket_tool.cc, http_conn.cc, http_conn_*.cc)
│   └── server/webserver.cc
├── static/                     # 静态网页资源(html/图片/媒体)
└── test/                       # webbench压测源码
    ├── webbench.c
    └── socket.c
```

## 编译步骤

### 环境要求

- **OS**: Linux (Ubuntu 18.04+ / CentOS 7+)
- **Compiler**: g++ 7.0+ (支持 C++11)
- **CMake**: 3.10+
- **MySQL**: libmysqlclient-dev (编译依赖，运行需 MySQL 服务)

### 安装依赖

```bash
# Ubuntu/Debian
sudo apt update
sudo apt install -y g++ cmake make libmysqlclient-dev

# CentOS/RHEL
sudo yum install -y gcc-c++ cmake make mysql-devel
```

### 准备数据库

```sql
-- 登录 MySQL
mysql -u root -p

-- 创建数据库
CREATE DATABASE db;
USE db;

-- 创建用户表
CREATE TABLE user (
    username VARCHAR(100) PRIMARY KEY,
    passwd   VARCHAR(100) NOT NULL
);

-- 可选：插入测试用户
INSERT INTO user VALUES ('admin', '123456');
```

### 编译

```bash
cd TinyWebServer
mkdir build && cd build
cmake ..
make -j$(nproc)
```

编译产物：
- `build/TinyWebServer` —— 主服务程序
- `build/webbench` —— 压测工具
- `build/static/` —— 自动拷贝的静态资源目录

## 启动服务

```bash
# 默认配置启动（端口 9006，Proactor 模式，LT+LT）
./TinyWebServer

# 自定义参数
./TinyWebServer -p 8080 -t 16 -s 16 -m 3 -a 1
```

### 启动参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-p <port>` | 监听端口 (1-65535) | 9006 |
| `-l <0/1>` | 日志模式：0同步 1异步 | 0 |
| `-m <0-3>` | epoll触发组合：0 LT+LT, 1 LT+ET, 2 ET+LT, 3 ET+ET | 0 |
| `-o <0/1>` | TCP优雅关闭 linger | 0 |
| `-s <num>` | 数据库连接池大小 | 8 |
| `-t <num>` | 线程池工作线程数 | 8 |
| `-c <0/1>` | 关闭日志：1关闭 0开启 | 0 |
| `-a <0/1>` | 并发模型：0 Proactor, 1 Reactor | 0 |

### 访问测试

浏览器访问 `http://<服务器IP>:9006`，即可看到登录/注册判断界面。

## 压力测试

### webbench 用法

```bash
# 1000 并发，持续 30 秒
./webbench -c 1000 -t 30 http://127.0.0.1:9006/

# 5000 并发，持续 60 秒
./webbench -c 5000 -t 60 http://127.0.0.1:9006/
```

### 实测结果 (Ubuntu 26.04, g++ 14.2.0, MySQL 8.0)

```bash
# 1000 并发 30 秒
$ ./webbench -c 1000 -t 30 http://127.0.0.1:9006/
Webbench - Simple Web Benchmark 1.5
Copyright (c) Radim Kolar 1997-2004, GPL Open Source Software.

Benchmarking: GET http://127.0.0.1:9006/
1000 clients, running 30 sec.
Speed=22326 pages/min, 41675 bytes/sec.
Requests: 11163 susceed, 0 failed.

# 5000 并发 60 秒
$ ./webbench -c 5000 -t 60 http://127.0.0.1:9006/
Webbench - Simple Web Benchmark 1.5
Copyright (c) Radim Kolar 1997-2004, GPL Open Source Software.

Benchmarking: GET http://127.0.0.1:9006/
5000 clients, running 60 sec.
Speed=45314 pages/min, 84586 bytes/sec.
Requests: 45314 susceed, 0 failed.
```

> **注意**：压测前请调整系统 fd 限制：`ulimit -n 65535`

## 技术要点

- **IO 模型**：epoll ET/LT 边缘/水平触发，支持 EPOLLONESHOT 防竞态
- **并发模型**：Proactor（主线程读，线程池处理 + 写）/ Reactor（线程池读 + 处理 + 写）
- **数据库**：RAII 连接池 + 单例用户缓存，MySQL 预加载 + 注册/登录业务
- **定时器**：升序双向链表，SIGALRM 驱动，惰性超时剔除空闲连接
- **HTTP**：状态机解析请求行/头部/正文，支持 GET 静态资源 + POST CGI
- **内存**：mmap 零拷贝文件映射 + writev 分散写响应头/文件体
- **日志**：同步/异步两种模式，异步基于阻塞队列 + 后台线程批量落盘

## License

MIT
