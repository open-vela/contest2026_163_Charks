#!/bin/sh
# 修复 R6 首轮编译失败 + 应用屏幕方案 A
#
# 首轮失败原因（链接错误）：
#   undefined reference to `baidu_tts_register' / `baidu_asr_register'
#   -> 只把 .c 拷进 src/voice/ 不够，还必须在 Makefile 与 CMakeLists.txt 里登记。
#      （volc_asr.c 就是这样登记的：Makefile:123 / CMakeLists.txt:63）
#
# 本脚本做三件事（全部幂等）：
#   1. 把 baidu_auth/asr/tts + mic_route + child_face 登记进 Makefile
#   2. 同样登记进 CMakeLists.txt
#   3. 屏幕方案 A：关掉 CONFIG_LCD_FRAMEBUFFER，让 LCD 回到 /dev/lcd0
#
# 用法：
#   bash /root/openvela-build/scripts/fix_r6_build.sh

set -u

AI=/root/openvela/packages/ai_agent
MK="$AI/Makefile"
CM="$AI/CMakeLists.txt"
DEF=/root/openvela/vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay/defconfig

SRCS="baidu_auth.c baidu_asr.c baidu_tts.c mic_route.c"
UI_SRC="child_face.c"

echo "=================================================================="
echo " 修复构建登记 + 屏幕方案 A"
echo "=================================================================="

# ---------------------------------------------------------------- 1. Makefile
echo ""
echo "--- 1. Makefile (CSRCS) ---"
[ -f "$MK" ] || { echo "  [错误] 找不到 $MK"; exit 1; }
cp -n "$MK" "$MK.r6bak" 2>/dev/null

for s in $SRCS; do
    if grep -q "src/voice/$s" "$MK"; then
        echo "  [跳过] src/voice/$s 已登记"
    else
        # 插在 volc_asr.c 那一行之后（与之同级）
        awk -v name="$s" '
            { print }
            /CSRCS \+= src\/voice\/volc_asr\.c/ {
                print "CSRCS += src/voice/" name
            }
        ' "$MK" > "$MK.tmp" && mv "$MK.tmp" "$MK"
        echo "  [增] CSRCS += src/voice/$s"
    fi
done

for s in $UI_SRC; do
    if grep -q "src/ui/$s" "$MK"; then
        echo "  [跳过] src/ui/$s 已登记"
    else
        awk -v name="$s" '
            { print }
            /CSRCS \+= src\/voice\/volc_asr\.c/ {
                print "CSRCS += src/ui/" name
            }
        ' "$MK" > "$MK.tmp" && mv "$MK.tmp" "$MK"
        echo "  [增] CSRCS += src/ui/$s"
    fi
done

# ---------------------------------------------------------------- 2. CMakeLists
echo ""
echo "--- 2. CMakeLists.txt ---"
if [ ! -f "$CM" ]; then
    echo "  [警告] 找不到 $CM（若用 make 构建可忽略）"
else
    cp -n "$CM" "$CM.r6bak" 2>/dev/null
    for s in $SRCS; do
        if grep -q "src/voice/$s" "$CM"; then
            echo "  [跳过] src/voice/$s 已登记"
        else
            awk -v name="$s" '
                { print }
                /src\/voice\/volc_asr\.c/ {
                    print "    src/voice/" name
                }
            ' "$CM" > "$CM.tmp" && mv "$CM.tmp" "$CM"
            echo "  [增] src/voice/$s"
        fi
    done
    for s in $UI_SRC; do
        if grep -q "src/ui/$s" "$CM"; then
            echo "  [跳过] src/ui/$s 已登记"
        else
            awk -v name="$s" '
                { print }
                /src\/voice\/volc_asr\.c/ {
                    print "    src/ui/" name
                }
            ' "$CM" > "$CM.tmp" && mv "$CM.tmp" "$CM"
            echo "  [增] src/ui/$s"
        fi
    done
fi

# ---------------------------------------------------------------- 3. 屏幕方案 A
echo ""
echo "--- 3. 屏幕方案 A：关掉 LCD_FRAMEBUFFER，回到 /dev/lcd0 ---"
echo "    依据：板上有 /dev/fb0 但无 /dev/lcd0；"
echo "          luncher_mini(预编译) 与多个 demo 写死找 /dev/lcd0"
if [ -f "$DEF" ]; then
    if grep -q "^CONFIG_LCD_FRAMEBUFFER=y" "$DEF"; then
        sed -i 's|^CONFIG_LCD_FRAMEBUFFER=y|# CONFIG_LCD_FRAMEBUFFER is not set|' "$DEF"
        echo "  [关] CONFIG_LCD_FRAMEBUFFER"
    elif grep -q "^# CONFIG_LCD_FRAMEBUFFER is not set" "$DEF"; then
        echo "  [跳过] 已经是关闭状态"
    else
        echo "# CONFIG_LCD_FRAMEBUFFER is not set" >> "$DEF"
        echo "  [增] # CONFIG_LCD_FRAMEBUFFER is not set"
    fi
    echo ""
    echo "  --- 当前显示相关值 ---"
    grep -E "LCD_FRAMEBUFFER|LCD_EXTERNINIT|LCD_DEV|LCD_ILI9341|CONFIG_LCD=" "$DEF"
else
    echo "  [错误] 找不到 $DEF"
fi

echo ""
echo "=================================================================="
echo " 完成。重跑构建："
echo "   cd /root/openvela-build && BOARD_CONFIG=configs/nsh_minidisplay \\"
echo "       bash scripts/remote_build_job.sh start"
echo "=================================================================="
