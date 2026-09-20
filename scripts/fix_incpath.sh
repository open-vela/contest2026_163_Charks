#!/bin/sh
# 给 packages/ai_agent 补上 aw-alsa-lib 的头文件搜索路径
#
# 问题
# ----
# 第三轮构建报：
#   src/voice/mic_route.c:22:10: fatal error: aw-alsa-lib/control.h: No such file or directory
#
# mic_route.c 用的是 #include <aw-alsa-lib/control.h>，
# 该头文件实际位于：
#   /root/openvela/vendor/allwinnertech/chips/r528/drivers/rtos-hal/include/hal/aw-alsa-lib/control.h
#
# 所以需要 -I .../rtos-hal/include/hal
#
# Makefile 第 38 行已有一条类似的：
#   CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/vendor/allwinnertech/.../rtos-hal/include/hal
# 但它没生效 —— $(APPDIR)/vendor 并不存在（vendor 是 apps 的兄弟目录，不是子目录）。
# 因此这里补一条**绝对路径**，最稳。
#
# 幂等：已存在就跳过。
# 用法： bash /root/openvela-build/scripts/fix_incpath.sh

set -u

AI=/root/openvela/packages/ai_agent
MK="$AI/Makefile"
INCDIR=/root/openvela/vendor/allwinnertech/chips/r528/drivers/rtos-hal/include/hal
LINE="CFLAGS += -I$INCDIR"

echo "=================================================================="
echo " 补 aw-alsa-lib 头文件路径"
echo "=================================================================="

[ -f "$MK" ] || { echo "[错误] 找不到 $MK"; exit 1; }

# 确认头文件确实在这里
if [ -f "$INCDIR/aw-alsa-lib/control.h" ]; then
    echo "  [OK] 头文件存在: $INCDIR/aw-alsa-lib/control.h"
else
    echo "  [错误] 头文件不在预期位置: $INCDIR/aw-alsa-lib/control.h"
    exit 1
fi

# 幂等检查
if grep -q "I$INCDIR" "$MK"; then
    echo "  [跳过] 已存在该 -I 行"
else
    cp -n "$MK" "$MK.incbak" 2>/dev/null
    awk -v inc="$LINE" '
        { print }
        /^CSRCS \+= src\/voice\/mic_route\.c/ && !done {
            print inc
            done = 1
        }
    ' "$MK" > "$MK.tmp" && mv "$MK.tmp" "$MK"
    echo "  [已加] $LINE"
fi

# 校验
echo ""
echo "  --- 校验：Make.defs/Makefile 里所有 rtos-hal 相关的 -I ---"
grep -n "rtos-hal/include/hal" "$MK"

echo ""
echo "  --- 校验：mic_route.c 的登记行附近 ---"
grep -n -B2 "src/voice/mic_route.c" "$MK"

echo ""
echo "完成。"
