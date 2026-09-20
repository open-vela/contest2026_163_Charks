#!/bin/bash
############################################################################
# scripts/remote_build_job.sh
#
# 在远端构建机上启停/查看构建任务。
#
# 存在的理由：用 ssh 内联命令来启动后台构建非常容易踩两个坑，我都踩过：
#
#   1) pkill -f build_openvela 会匹配到"执行这条命令的 shell 自己"，
#      于是命令把自己杀掉，构建根本没启动（表现为 ssh 返回空输出）。
#      这里一律用 build_openve[l]a 这种括号写法，让正则不匹配自身命令行。
#
#   2) ssh 里嵌套引号容易被本地 shell 提前吃掉，导致远端命令被拆散。
#      所以把逻辑写进文件、用 scp 传过去执行，而不是拼一行命令。
#
# 用法：
#   bash scripts/remote_build_job.sh start   [日志文件]
#   bash scripts/remote_build_job.sh stop
#   bash scripts/remote_build_job.sh status
############################################################################

set -u

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SELF_DIR")"
BUILD_DIR="${BUILD_DIR:-/root/openvela-build}"
LOG="${2:-/root/build.out}"

# 正则用括号写法，避免匹配到本脚本自身的命令行
PATTERN='build_openve[l]a'

start_job() {
    if pgrep -f "$PATTERN" >/dev/null 2>&1; then
        echo "已有构建在运行，pid=$(pgrep -f "$PATTERN" | head -1)。如需重来请先 stop。"
        return 1
    fi

    # 保留上一轮日志，便于对比
    if [ -f "$LOG" ]; then
        cp "$LOG" "${LOG}.prev" 2>/dev/null
    fi

    cd "$BUILD_DIR" || { echo "构建目录不存在: $BUILD_DIR"; return 1; }

    # BOARD_CONFIG 透传给主脚本（默认 configs/nsh，可用环境变量改）
    # setsid + nohup + 三个重定向：让进程脱离 ssh 会话，ssh 断开也不受影响
    setsid nohup env MIN_DISK_GB="${MIN_DISK_GB:-8}" \
        BOARD_CONFIG="${BOARD_CONFIG:-configs/nsh}" \
        bash scripts/build_openvela.sh > "$LOG" 2>&1 < /dev/null &

    sleep 5
    if pgrep -f "$PATTERN" >/dev/null 2>&1; then
        echo "已启动，pid=$(pgrep -f "$PATTERN" | head -1)，日志: $LOG"
    else
        echo "启动失败，请查看 $LOG"
        tail -20 "$LOG" 2>/dev/null
        return 1
    fi
}

stop_job() {
    if pgrep -f "$PATTERN" >/dev/null 2>&1; then
        pkill -f "$PATTERN"
        sleep 2
        echo "已停止"
    else
        echo "没有在运行的构建"
    fi
}

status_job() {
    if pgrep -f "$PATTERN" >/dev/null 2>&1; then
        echo "运行中  pid=$(pgrep -f "$PATTERN" | head -1)"
        echo "已运行  $(ps -o etimes= -p "$(pgrep -f "$PATTERN" | head -1)" 2>/dev/null | tr -d ' ') 秒"
    else
        echo "未运行"
    fi
    echo "日志: $(ls -l --time-style=+%m-%d\ %H:%M:%S "$LOG" 2>/dev/null || echo 不存在)"
    echo "磁盘: $(df -BG / | tail -1)"
    echo "--- 日志末尾 ---"
    tail -6 "$LOG" 2>/dev/null
}

case "${1:-status}" in
    start)  start_job ;;
    stop)   stop_job ;;
    status) status_job ;;
    *)      echo "用法: $0 {start|stop|status} [日志文件]"; exit 2 ;;
esac
