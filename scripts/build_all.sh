#!/bin/bash
# export https_proxy=http://127.0.0.1:8123
# 检查是否已经安装了 ESP-IDF 环境
if [ -z "$IDF_PATH" ]; then
    source ~/esp/v6.0.2/esp-idf/export.sh
fi

# 设置日志文件
# 使用项目根目录绝对路径，避免依赖执行时的当前目录：
# build_all.py 内部会 chdir 到项目根目录，build_summary.log 实际由它写在根目录，
# 若这里用相对路径，清空的是 scripts/ 下的空壳，看到的永远是空文件。
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOG_FILE="$PROJECT_ROOT/build.log"
SUMMARY_FILE="$PROJECT_ROOT/build_summary.log"

###############################################################################
# build_all.py 支持的模式
#
# 用法（python scripts/build_all.py <参数>）：
#   build_all.py all                    编译所有板子（默认）
#   build_all.py all --target esp32c3 --out-dir releases/esp32c3 只编译指定芯片的所有板子：esp32c3|esp32|esp32s3|esp32p4，编译并输出到指定目录
#   build_all.py xmini/c3               编译单个板子的【全部】variant
#   build_all.py xmini/c3 --name xmini-c3   只编译该板子下 name 为 xmini-c3 的那一个 variant
#   build_all.py --list-boards          列出所有支持的板子
#   build_all.py (无参数)                打包当前 build/ 已编译的固件
#
#   ⚠ 注意：不带 --out-dir 时，zip 默认输出到 releases/ 根目录。
#   例如默认 `build_all.py all` 会把所有板的固件都放到 releases/，
#   如需按芯片分目录，请用：all --target <芯片> --out-dir releases/<芯片>
###############################################################################

# 默认运行模式：编译所有板子；要使用其他模式，将对应的 MODE 变量取消注释
# MODE="all"
# MODE="xmini/c3"                 # 编译单个板子的【全部】variant
# MODE="xmini/c3 --name xmini-c3" # 只编译该板子下 name 为 xmini-c3 的那一个 variant
# MODE="all --target esp32c3 --out-dir releases/esp32c3"
# MODE="all --target esp32 --out-dir releases/esp32"
# MODE="all --target esp32s3 --out-dir releases/esp32s3"
# MODE="all --target esp32p4 --out-dir releases/esp32p4"
# MODE="--list-boards"

# 默认命令：python build_all.py all（后台运行，输出到日志）
# 同样用绝对路径，这样无论在哪个目录执行 build_all.sh 都能找到 build_all.py
# （build_all.py 内部会自行 chdir 到项目根目录，故调用时的 CWD 不影响其行为）
# 用数组而非字符串：字符串里的转义引号在展开后只会变成字面字符，不会起到引用作用
RUN_CMD=(python "$PROJECT_ROOT/scripts/build_all.py")
if [ -n "${MODE:-}" ]; then
    # MODE 里含多个参数（如 "all --target esp32s3"），此处需要按空格拆分成独立元素
    # shellcheck disable=SC2206
    RUN_CMD+=($MODE)
fi

# 清空上一次的汇总日志，避免累积
> "$SUMMARY_FILE"

# 后台运行构建过程并将输出重定向到日志文件
nohup "${RUN_CMD[@]}" > "$LOG_FILE" 2>&1 &

# 获取后台进程 PID
BUILD_PID=$!

echo "构建任务已在后台启动 (PID: $BUILD_PID)"
echo "命令: ${RUN_CMD[*]}"
echo "完整日志: $LOG_FILE"
echo "结果汇总: $SUMMARY_FILE"
echo "日志: $LOG_FILE"
