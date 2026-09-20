#!/usr/bin/env bash
# ============================================================================
# 在 ECS 上一键跑完整固件构建（集成 + 编译 + 打包），日志留档。
#
# 为什么要包一层
# --------------
# 编译要 40 分钟以上。ssh 直接跑会：
#   · 被客户端超时掐断（连接一断，子进程收到 SIGHUP 就死了）
#   · 日志滚出屏幕，回头找不到第一个 error
# 所以统一 nohup + 落盘，另配一个 tail 看进度的方式。
#
# 用法：
#     nohup bash scripts/ecs_build.sh > /root/fw_build.log 2>&1 &
#     tail -f /root/fw_build.log
# ============================================================================

set -uo pipefail

SRC="${OPENVELA_SRC:-/root/openvela}"
BUILD="${SUPERCHILD_BUILD:-/root/build}"
BOARD_CONFIG="${BOARD_CONFIG:-configs/nsh_minidisplay}"
JOBS="${JOBS:-4}"
LOG=/root/fw_build.log

step() { echo; echo "=== $* === $(date -Is)"; }

step "0 集成板端代码"
OPENVELA_SRC="$SRC" bash "$BUILD/scripts/integrate_superchild.sh" 2>&1 | tail -20

step "1 编译 + 打包（这一步很久，40 分钟起）"
cd "$BUILD" || exit 1
OPENVELA_SRC="$SRC" \
BOARD_CONFIG="$BOARD_CONFIG" \
JOBS="$JOBS" \
BUILD_FLAGS="-e -Wno-error" \
bash "$BUILD/scripts/build_openvela.sh"

step "2 收集产物"
find "$SRC/vendor/allwinnertech/lichee" -maxdepth 5 -type f -name '*.img' \
     -newermt '-6 hours' -printf '%10s  %TY-%Tm-%Td %TH:%TM  %p\n' 2>/dev/null \
     | sort -k3 | tail -10

echo
echo "构建结束 $(date -Is)"
