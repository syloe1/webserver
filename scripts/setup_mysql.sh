#!/bin/bash
set -e

echo "=== 1. 停止现有 MySQL ==="
sudo systemctl stop mysql 2>/dev/null || true
sudo pkill mysqld 2>/dev/null || true
sleep 1

echo "=== 2. 创建 socket 目录 ==="
sudo mkdir -p /var/run/mysqld
sudo chown mysql:mysql /var/run/mysqld

echo "=== 3. 以 skip-grant-tables 模式启动 MySQL ==="
sudo mysqld --skip-grant-tables --user=mysql --datadir=/var/lib/mysql &
MYSQLD_PID=$!
sleep 3

echo "=== 4. 重置 root 密码 ==="
mysql -u root <<'SQL'
FLUSH PRIVILEGES;
ALTER USER 'root'@'localhost' IDENTIFIED WITH caching_sha2_password BY 'qaz123';
FLUSH PRIVILEGES;
SQL
echo "root 密码已重置为 qaz123"

echo "=== 5. 停止 skip-grant-tables 实例 ==="
sudo kill $MYSQLD_PID
sleep 2

echo "=== 6. 正常启动 MySQL ==="
sudo systemctl start mysql
sleep 2

echo "=== 7. 验证连接 ==="
mysql -u root -pqaz123 -e "SELECT 'MySQL connection OK!' AS status;"

echo "=== 8. 创建项目数据库和表 ==="
mysql -u root -pqaz123 <<'SQL'
CREATE DATABASE IF NOT EXISTS db;
USE db;
CREATE TABLE IF NOT EXISTS user (
    id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(64) NOT NULL UNIQUE,
    passwd  VARCHAR(64) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
INSERT IGNORE INTO user (username, passwd) VALUES ('admin', 'admin123');
SQL

echo ""
echo "=== 全部完成 ==="
echo "数据库: db"
echo "表:     user"
echo "测试账号: admin / admin123"
