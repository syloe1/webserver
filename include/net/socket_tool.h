#ifndef SOCKET_TOOL_H
#define SOCKET_TOOL_H

// ============================================================
// 底层socket工具函数 —— 全项目通用，与单HTTP连接无关
// WebServer、timer、http_conn 均可复用
// ============================================================

// 对文件描述符设置非阻塞，返回旧的文件状态标志
int setnonblocking(int fd);

// 将内核事件表注册读事件，支持ET/LT模式，可选开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

// 从内核事件表删除描述符，并关闭fd
void removefd(int epollfd, int fd);

// 将事件重置为EPOLLONESHOT（用于读写完成后重新挂载监听）
void modfd(int epollfd, int fd, int ev, int TRIGMode);

#endif
