#!/bin/sh
# 诊断：R2（有屏）与当前 AI Agent（无屏）两份 defconfig 的差异，聚焦显示相关项
#
# 背景
# ----
# 现在的固件屏幕是"白屏 + 红条 80%" —— 根因是 /dev/lcd0 不存在，
# LVGL 初始化失败。而 R2 固件是能亮的，说明差异就在 defconfig 里。
#
# prepare_r5_config.sh 在切换成 AI Agent 配置时留了备份 defconfig.orig，
# 本脚本就拿它和当前版本做 diff，把显示相关的增删项列出来。
#
# 用法（在云主机上）：
#   bash /root/openvela-build/scripts/diag_display.sh

set -u

D=/root/openvela/vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay

cd "$D" || { echo "[错误] 找不到 $D"; exit 1; }

echo "=================================================================="
echo " 显示配置差异诊断"
echo "=================================================================="
echo ""
echo "--- 文件 ---"
ls -l defconfig defconfig.orig 2>/dev/null
echo ""

if [ ! -f defconfig.orig ]; then
    echo "[警告] 没有 defconfig.orig 备份，无法对比"
    echo "       可尝试： cd /root/openvela/vendor/allwinnertech && git diff"
    exit 1
fi

PAT='lcd|disp|fb|panel|ili|spi|backlight|lvgl|touch|framebuf|rgb|mipi'

echo "--- ① 被移除的（只在 R2 有，当前没有）—— 屏幕失效的嫌疑项 ---"
diff defconfig.orig defconfig | grep '^<' | grep -iE "$PAT"
echo ""

echo "--- ② 新增的（只在当前有，R2 没有）---"
diff defconfig.orig defconfig | grep '^>' | grep -iE "$PAT"
echo ""

echo "--- ③ 被关闭的（从 =y 变成 # ... is not set）---"
diff defconfig.orig defconfig | grep '^<' | grep '=y' | grep -iE "$PAT"
echo ""

echo "--- ④ 差异总量（不含上面过滤）---"
diff defconfig.orig defconfig | grep -c '^[<>]' | sed 's/^/    共 /;s/$/ 行差异/'
echo ""

echo "--- ⑤ 当前配置里显示相关项的实际值 ---"
grep -iE "$PAT" defconfig | grep -E '=y|=[0-9]|is not set' | head -25
echo ""

echo "--- ⑥ AI Agent 官方 defconfig（若存在）里的显示项 ---"
AI=/root/openvela/packages/ai_agent/defconfigs
if [ -d "$AI" ]; then
    ls "$AI"
    for f in "$AI"/*/defconfig; do
        [ -f "$f" ] || continue
        echo "  == $f =="
        grep -iE "$PAT" "$f" | grep -E '=y|is not set' | head -15
    done
else
    echo "  （无 $AI）"
fi
