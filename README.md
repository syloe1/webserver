# TinyWebServer

高性能 C++11 多线程 Web 服务器。当前主分支基于 **io_uring** 异步 I/O，[main](https://github.com/syloe1/webserver/tree/main) 分支保留 **epoll** 版本用于对比。

```
I/O 引擎：io_uring (SQ/CQ 环形队列)  ← 当前分支
并发模型：Proactor（主线程统一提交 SQ，线程池处理业务）
辅助组件：MySQL 连接池 + 升序定时器链表 + 同步/异步日志
```

## io_uring vs epoll 对比

| | epoll（main 分支） | io_uring（当前分支） |
|---|---|---|
| **I/O 模型** | 就绪通知：epoll_wait 告知 fd 可读/写，用户态调 recv/send | 完成通知：提交 SQ，内核异步完成 I/O，CQE 返回结果 |
| **系统调用** | 每次请求 ~4 次（epoll_wait + recv + modfd + writev） | 批量提交+收割，1 次 io_uring_enter 完成多操作 |
| **accept** | 循环 accept() 直到 EAGAIN | IORING_ACCEPT_MULTISHOT：一个 SQE 持久生效 |
| **线程安全** | epoll_ctl 天然线程安全 | Worker 只设标记 + pipe 唤醒，主线程统一提交 SQ |
| **内核依赖** | Linux 2.6+ | Linux 5.1+ (multishot accept 需 6.1+) |
| **内存拷贝** | 每次 recv/send 用户态↔内核态各 1 次 | 支持 registered buffers 零拷贝（待实现） |

### 架构对比

```
epoll 流程：
  epoll_wait → "fd 可读了" → recv() → parse → modfd(EPOLLOUT)
  epoll_wait → "fd 可写了" → writev() → modfd(EPOLLIN) → 循环

io_uring 流程：
  submit_and_wait(1) → ACCEPT CQE → submit_recv
  RECV CQE → threadpool → process() → worker 设标记 + pipe 唤醒主线程
  主线程 flush → submit_send → SEND CQE → on_send_done → 循环
```

### 压测对比（同一机器，Ubuntu 26.04, kernel 7.0）

#### webbench（fork-per-client，误差较大）

| 并发 | epoll (main) | io_uring (当前) |
|:---:|------:|------:|
| 1000 | 128,930 pages/min | **127,014** pages/min |
| 5000 | 132,782 pages/min, 2 失败 | **122,048 pages/min, 0 失败** |

#### wrk（事件驱动，更精确）

| 并发 | QPS | 延迟(avg) | 错误 |
|:---:|------:|------:|:---:|
| 10 | 2,140 | 4.6ms | 0 |
| 100 | 2,044 | 48.5ms | 0 |
| 500 | 2,075 | 236ms | 0 |
| 1000 | 2,028 | 478ms | 0 |

> io_uring 版：**全并发 0 错误**，epoll 版 5000 并发出现 2 次失败。
> io_uring 异步模型无 fd 事件注册，无需 modfd 反复修改监听事件，稳定性更强。
> 目前瓶颈在线程池（8 workers），不是 I/O。registered buffers + fixed files 等零拷贝优化可进一步提吞吐。

## 项目结构

```
TinyWebServer/
├── CMakeLists.txt
├── main.cc
├── include/
│   ├── config/config.h
│   ├── core/
│   │   ├── locker.h              # 锁/信号量/条件变量/RAII
│   │   ├── log.h                 # 同步/异步日志
│   │   ├── block_queue.h         # 线程安全阻塞队列
│   │   ├── lst_timer.h           # 升序双向定时器链表 + Utils
│   │   ├── threadpool.h          # 线程池模板
│   │   └── io_uring_engine.h     # io_uring 最小封装（替代 epoll）
│   ├── db/
│   │   ├── sql_connection_pool.h # MySQL 连接池(单例)
│   │   └── user_cache.h          # 用户缓存(单例)
│   ├── net/
│   │   ├── socket_tool.h         # fd 工具（setnonblocking）
│   │   ├── http_const.h          # HTTP 状态码/错误页
│   │   └── http_conn.h           # HTTP 连接(解析+路由+响应)
│   └── server/webserver.h        # 顶层总控
├── src/                          # 实现文件（与 include 对应）
├── static/                       # 静态资源
└── test/
    └── webbench.c                # 压测工具
```

## 编译

### 环境要求

- **OS**: Linux (Ubuntu 20.04+, 内核 5.1+)
- **Compiler**: g++ 7.0+ (C++11)
- **CMake**: 3.10+
- **liburing**: io_uring 用户态库
- **MySQL**: libmysqlclient-dev

### 安装依赖

```bash
# 基础工具
sudo apt install -y g++ cmake make

# io_uring (新增)
sudo apt install -y liburing-dev

# MySQL
sudo apt install -y libmysqlclient-dev mysql-server
```

### 准备数据库

（同 epoll 版本，见下方完整指南）

### 编译

```bash
cd TinyWebServer
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

编译产物：`build/TinyWebServer`、`build/webbench`

## 启动

```bash
# 默认配置（端口 9006, Proactor）
./TinyWebServer

# 自定义参数
./TinyWebServer -p 8080 -t 16 -s 16 -a 1
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-p <port>` | 监听端口 | 9006 |
| `-l <0/1>` | 日志模式：0同步 1异步 | 0 |
| `-o <0/1>` | TCP 优雅关闭 | 0 |
| `-s <num>` | 数据库连接池大小 | 8 |
| `-t <num>` | 线程池线程数 | 8 |
| `-c <0/1>` | 关闭日志 | 0 |
| `-a <0/1>` | 并发模型：0 Proactor, 1 Reactor | 0 |

> `-m` 参数已移除（io_uring 无 LT/ET 概念）

## 浏览器测试

```
http://<服务器IP>:9006/
```

## 压力测试

### wrk（推荐，事件驱动，资源占用低）

```bash
# 安装
sudo apt install -y wrk

# 先调大 fd 限制
ulimit -n 65535

# 100 并发
wrk -c 100 -t 4 -d 30s http://127.0.0.1:9006/judge.html

# 1000 并发
wrk -c 1000 -t 4 -d 30s http://127.0.0.1:9006/judge.html

# 5000 并发（需要 ulimit -n 65535）
wrk -c 5000 -t 8 -d 30s http://127.0.0.1:9006/judge.html
```

### webbench（兼容保留，fork-per-client，1000+ 并发慎用）

```bash
# 小并发（推荐）
./webbench -c 10 -t 30 http://127.0.0.1:9006/judge.html

# 高并发（需要 ulimit -n 65535，可能影响同机其他进程）
./webbench -c 1000 -t 30 http://127.0.0.1:9006/judge.html
```

> webbench 缺陷：fork 进程模型，1000 并发 = 1000 进程，CPU/内存开销大，
> 容易挤占 VSCode 等开发工具资源导致卡顿。高并发建议用 wrk 或 ab。

### io_uring wrk 实测（Ubuntu 26.04, kernel 7.0）

| 并发 | QPS | 延迟 | 错误 |
|:---:|------:|------:|:---:|
| 10 | 2,140 | 4.6ms | 0 |
| 100 | 2,044 | 48.5ms | 0 |
| 500 | 2,075 | 236ms | 0 |
| 1000 | 2,028 | 478ms | 0 |

### epoll webbench 实测（同机器，main 分支）

| 并发 | QPS | 错误 |
|:---:|------:|:---:|
| 1000 | 128,930 pages/min | 0 |
| 5000 | 132,782 pages/min | 2 |

## 技术要点

- **I/O 引擎**：io_uring SQ/CQ 环形队列，multishot accept，单线程统一提交
- **并发模型**：Proactor — 主线程收割 CQE + 提交 SQ，线程池处理 HTTP 业务
- **线程安全**：Worker 只设标记 + 写入 pipe 唤醒主线程，避免多线程竞争 io_uring SQ
- **数据库**：RAII 连接池 + 单例用户缓存
- **定时器**：升序双向链表，SIGALRM 驱动，惰性剔除空闲连接
- **HTTP**：状态机解析，支持 GET 静态资源 + POST CGI 登录/注册
- **内存**：mmap 文件映射 + writev 分散写（header + file）
- **日志**：同步/异步两种模式，异步基于阻塞队列 + 后台线程落盘

---

## 数据库配置（完整指南）

<details>
<summary>点击展开</summary>

### 1. 安装 MySQL Server

```bash
sudo apt install -y mysql-server
```

### 2. 启动

```bash
sudo systemctl start mysql
sudo systemctl enable mysql
```

### 3. 配置密码认证

```bash
sudo mysql
```

```sql
ALTER USER 'root'@'localhost' IDENTIFIED WITH mysql_native_password BY '你的密码';
FLUSH PRIVILEGES;
EXIT;
```

```bash
mysql -u root -p   # 验证密码登录
```

### 4. 建库建表

```sql
CREATE DATABASE db;
USE db;
CREATE TABLE user (
    username VARCHAR(100) PRIMARY KEY,
    passwd   VARCHAR(100) NOT NULL
);
INSERT INTO user VALUES ('admin', '123456');
```

### 5. 修改 main.cc

```cpp
const string DB_USER = "root";
const string DB_PASSWD = "你的密码";
const string DB_NAME = "db";
```

修改后重新编译：`cd build && make -j$(nproc)`

</details>

## License

MIT
