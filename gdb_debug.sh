#!/bin/bash
# GDB 调试 TinyWebServer — 捕获崩溃现场
ulimit -c unlimited
echo "core size: $(ulimit -c)"

# 方式1: 直接 GDB 运行
echo "=== 启动 GDB ==="
gdb -ex "set pagination off" \
    -ex "handle SIGPIPE nostop noprint" \
    -ex "handle SIGALRM nostop noprint" \
    -ex "handle SIGTERM nostop noprint" \
    -ex "run -p 9006" \
    -ex "bt full" \
    -ex "info threads" \
    -ex "frame 0" \
    ./build/TinyWebServer
