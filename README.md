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

#### 1. 安装 MySQL Server

编译时只需要客户端库 (`libmysqlclient-dev`)，但**运行时必须有 MySQL 服务**。如果本机没有安装，先执行：

```bash
# Ubuntu/Debian
sudo apt install -y mysql-server

# CentOS/RHEL
sudo yum install -y mysql-server
```

#### 2. 启动 MySQL 服务

```bash
# Systemd 系统（Ubuntu 18.04+ / CentOS 7+）
sudo systemctl start mysql
sudo systemctl enable mysql   # 设为开机自启

# 检查服务状态
sudo systemctl status mysql
```

#### 3. 配置密码认证（重要）

Ubuntu 新装 MySQL 默认使用 `auth_socket` 插件 —— 这意味着只有系统 root 用户才能通过 `sudo mysql` 登录，**密码登录被禁用**。程序通过 TCP 连接数据库必须使用密码认证，需要先修改认证方式。

```bash
# Step 1: 用 socket 方式登录（无需密码）
sudo mysql

# Step 2: 查看当前 root 的认证插件（确认是否为 auth_socket）
SELECT user, host, plugin FROM mysql.user WHERE user='root';
# 如果你看到 plugin 列显示 "auth_socket"，说明需要修改
```

然后执行以下 SQL 切换到密码认证：

```sql
-- 将 root 用户的认证插件改为 mysql_native_password，同时设置密码
ALTER USER 'root'@'localhost' IDENTIFIED WITH mysql_native_password BY '你的密码';

-- 刷新权限使修改生效
FLUSH PRIVILEGES;

-- 验证：退出后用密码重新登录
EXIT;
```

```bash
# 用密码重新登录测试（注意：必须用 -p，不能用 sudo mysql）
mysql -u root -p
# 输入你刚设置的密码，能进入就说明配置成功
```

> **提示**：MySQL 8.0+ 默认使用 `caching_sha2_password` 插件。如果连接报错 `Authentication plugin 'caching_sha2_password' cannot be loaded`，请改用 `mysql_native_password` 插件（如上所示），兼容性最好。

#### 4. 创建数据库和表

```sql
-- 登录 MySQL（用密码方式）
mysql -u root -p

-- 创建数据库
CREATE DATABASE db;
USE db;

-- 创建用户表
CREATE TABLE user (
    username VARCHAR(100) PRIMARY KEY,
    passwd   VARCHAR(100) NOT NULL
);

-- 可选：插入测试用户（密码明文存储仅用于演示，生产环境请使用哈希）
INSERT INTO user VALUES ('admin', '123456');

-- 验证表是否创建成功
DESC user;
SELECT * FROM user;
```

#### 5. （推荐）创建专用数据库用户

用 root 直连有安全风险，建议创建一个仅对 `db` 库有权限的专用用户：

```sql
-- 创建用户并设置密码
CREATE USER 'webuser'@'localhost' IDENTIFIED WITH mysql_native_password BY 'webserver123';

-- 授予 db 数据库的全部权限
GRANT ALL PRIVILEGES ON db.* TO 'webuser'@'localhost';

-- 也可以只授予必要权限（更安全）
-- GRANT SELECT, INSERT, UPDATE ON db.* TO 'webuser'@'localhost';

FLUSH PRIVILEGES;

-- 验证新用户能否登录
EXIT;
```

```bash
mysql -u webuser -p
# 输入密码：webserver123
# 执行 USE db; SELECT * FROM user; 确认有权限
```

#### 6. 修改 main.cc 中的数据库配置

数据库创建完成后，需要将连接信息填入代码。打开 [main.cc](main.cc)，修改文件顶部的三个常量：

```cpp
// 数据库配置抽离，方便统一修改，后续可迁移到配置文件
const string DB_USER = "root";        // 改为你的数据库用户名
const string DB_PASSWD = "qaz123";    // 改为你设置的密码
const string DB_NAME = "db";          // 数据库名（默认 db 不用改）
```

**示例**：如果你按照上面的步骤用 root + 密码 `MyPass@2024`：

```cpp
const string DB_USER = "root";
const string DB_PASSWD = "MyPass@2024";
const string DB_NAME = "db";
```

如果创建了专用用户 `webuser`：

```cpp
const string DB_USER = "webuser";
const string DB_PASSWD = "webserver123";
const string DB_NAME = "db";
```

> **注意**：修改 `main.cc` 后需要**重新编译**才能生效：
> ```bash
> cd build && make -j$(nproc)
> ```

#### 7. 常见问题排查

| 现象 | 原因 | 解决 |
|------|------|------|
| `Can't connect to MySQL server on 'localhost'` | MySQL 服务未启动 | `sudo systemctl start mysql` |
| `Access denied for user 'root'@'localhost'` | 密码错误或仍用 auth_socket | 按第 3 步重设密码和认证插件 |
| `Unknown database 'db'` | 未创建数据库 | 按第 4 步执行 `CREATE DATABASE db;` |
| `Table 'db.user' doesn't exist` | 未创建表 | 按第 4 步执行建表语句 |
| `Authentication plugin 'xxx' cannot be loaded` | 认证插件不兼容 | 改用 `mysql_native_password`（见第 3 步提示） |
| 程序启动后数据库相关功能无响应 | 连接池配置过大或密码错误 | 检查 `main.cc` 中密码是否正确，或将 `-s` 参数调小 |

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
