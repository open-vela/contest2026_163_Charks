#!/bin/bash
############################################################################
# scripts/probe_headers.sh
#
# 探测：板级源文件里包含哪些头文件是安全的。
#
# 背景：往 vendor 板级目录加源文件时，`#include <syslog.h>` 会经
#   sys/time.h -> sys/select.h -> 报 OPEN_MAX / CLOCK_MAX 未定义
# 而同目录的 r528_bringup.c 用同样的头却能编译。与其反复跑两分钟的完整
# 构建去猜，不如直接用编译器把几种包含组合各探一遍。
#
# 用法：在 nuttx 目录下执行
#   bash scripts/probe_headers.sh
############################################################################

set -u

NUTTX=/root/openvela/nuttx
CC=/root/openvela/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin/arm-none-eabi-gcc
TMP=/tmp/hdrprobe

mkdir -p "$TMP"

# 与工程一致的包含路径（取自 nuttx 的构建配置）
INC="-I$NUTTX/include -I$NUTTX/arch/arm/include -I$NUTTX/arch/arm/src/common"

probe() {
    local name="$1"
    local body="$2"
    local f="$TMP/$name.c"

    printf '%s\n' "$body" > "$f"

    if $CC -fsyntax-only -std=gnu11 $INC "$f" 2>"$TMP/$name.err"; then
        echo "  [通过] $name"
    else
        echo "  [失败] $name"
        # 只打印前两条错误，避免刷屏
        grep -m2 "error:" "$TMP/$name.err" | sed 's/^/         /'
    fi
}

echo "=============================================================="
echo "  头文件组合探测"
echo "=============================================================="
echo "  gcc : $($CC -dumpversion)"
echo

probe "a_config_only" '#include <nuttx/config.h>
int probe_symbol_placeholder;'

probe "b_config_stdint" '#include <nuttx/config.h>
#include <stdint.h>
int probe_symbol_placeholder;'

probe "c_config_syslog" '#include <nuttx/config.h>
#include <stdint.h>
#include <syslog.h>
int probe_symbol_placeholder;'

probe "d_config_limits_syslog" '#include <nuttx/config.h>
#include <stdint.h>
#include <limits.h>
#include <syslog.h>
int probe_symbol_placeholder;'

probe "e_config_debug" '#include <nuttx/config.h>
#include <stdint.h>
#include <debug.h>
int probe_symbol_placeholder;'

probe "f_config_debug_syslog" '#include <nuttx/config.h>
#include <stdint.h>
#include <debug.h>
#include <syslog.h>
int probe_symbol_placeholder;'

echo
echo "（各组合的错误详情在 $TMP/*.err）"
