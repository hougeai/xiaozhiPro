#!/bin/bash
# export https_proxy=http://127.0.0.1:8123
# 检查是否已经安装了 ESP-IDF 环境
if [ -z "$IDF_PATH" ]; then
    source ~/esp/v6.0.2/esp-idf/export.sh
fi

# 设置日志文件
LOG_FILE="build.log"
SUMMARY_FILE="build_summary.log"

###############################################################################
# release.py 支持的模式
#
# 用法（python scripts/release.py <参数>）：
#   release.py all                    编译所有板子（默认）
#   release.py all --target esp32c3 --out-dir releases/esp32c3 只编译指定芯片的所有板子：esp32c3|esp32|esp32s3|esp32p4，编译并输出到指定目录
#   release.py xmini/c3               编译单个板子的默认 variant
#   release.py xmini/c3 --name xmini-c3   编译单个板子的指定 variant
#   release.py --list-boards          列出所有支持的板子
#   release.py (无参数)                打包当前 build/ 已编译的固件
#
#   ⚠ 注意：不带 --out-dir 时，zip 默认输出到 releases/ 根目录。
#   例如默认 `release.py all` 会把所有板的固件都放到 releases/，
#   如需按芯片分目录，请用：all --target <芯片> --out-dir releases/<芯片>
###############################################################################

# 默认运行模式：编译所有板子
# 要使用其他模式，修改下面的 MODE 变量，或直接替换 RUN_CMD
#MODE="all"
#MODE="xmini/c3"
#MODE="xmini/c3 --name xmini-c3"
#MODE="all --target esp32c3 --out-dir releases/esp32c3"
#MODE="all --target esp32 --out-dir releases/esp32"
#MODE="all --target esp32s3 --out-dir releases/esp32s3"
#MODE="all --target esp32p4 --out-dir releases/esp32p4"
#MODE="--list-boards"

# 默认命令：python release.py all（后台运行，输出到日志）
RUN_CMD="python release.py ${MODE:-all}"

# 清空上一次的汇总日志，避免累积
> "$SUMMARY_FILE"

# 后台运行构建过程并将输出重定向到日志文件
nohup $RUN_CMD > "$LOG_FILE" 2>&1 &

# 获取后台进程 PID
BUILD_PID=$!

echo "构建任务已在后台启动 (PID: $BUILD_PID)"
echo "命令: $RUN_CMD"
echo "完整日志: $LOG_FILE (tail -f $LOG_FILE 查看详细过程)"
echo "结果汇总: $SUMMARY_FILE (cat $SUMMARY_FILE 实时查看每块板成功/失败)"
echo "日志: $LOG_FILE (可用 'tail -f $LOG_FILE' 查看进度)"
