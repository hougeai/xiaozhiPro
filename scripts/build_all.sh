#!/bin/bash
# export https_proxy=http://127.0.0.1:8123
# 检查是否已经安装了 ESP-IDF 环境
if [ -z "$IDF_PATH" ]; then
    source ~/esp/v6.0.2/esp-idf/export.sh
fi

# 设置日志文件
LOG_FILE="build.log"

# 后台运行构建过程并将输出重定向到日志文件
nohup python release.py all > "$LOG_FILE" 2>&1 &

# 获取后台进程 PID
BUILD_PID=$!

echo "构建任务已在后台启动 (PID: $BUILD_PID)"