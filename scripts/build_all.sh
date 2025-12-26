#!/bin/bash

# 检查是否已经安装了 ESP-IDF 环境
if [ -z "$IDF_PATH" ]; then
    echo "错误: 请先设置 ESP-IDF 环境变量"
    echo "例如: export IDF_PATH=/path/to/esp-idf"
    exit 1
fi

# 设置日志文件
LOG_FILE="build.log"

# 后台运行构建过程并将输出重定向到日志文件
nohup python release.py all > "$LOG_FILE" 2>&1 &

# 获取后台进程 PID
BUILD_PID=$!

echo "构建任务已在后台启动 (PID: $BUILD_PID)"