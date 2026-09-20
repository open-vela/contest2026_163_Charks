#!/bin/sh
# 修复架构缺陷：麦克风采集通路只被百度后端打开
#
# 缺陷链条（详见《设计-闭环v2.md》决策 4）
# ----------------------------------------
#   mic_route_enable() 全项目唯一调用点是 baidu_asr.c 的 baidu_asr_init_ops()
#   → 只有切到「百度」ASR 后端时，麦克风输入通路才会被打开
#   → 用火山后端（AI Agent 默认）时，通路每次开机都是 Off
#   → 录音必然失败，报误导性的 "capture only support 1~3 channel"
#
# 而 mic_route.c 自己的头注释就写了：这是**板级/驱动层**问题，与百度无关。
# 所以正确位置是后端无关的 voice_channel_init()。
#
# 本脚本做两件事（都幂等）：
#   1. 在 voice_channel_init() 里调用 mic_route_enable()（在任何后端注册之前）
#   2. 确保 mic_route.c 已登记进 Makefile 的 CSRCS（否则链接失败）
#
# baidu_asr.c 里原有那处调用**保留不动** —— mic_route_enable() 是幂等的
# （已 On 的项会跳过），多处调用无害，且能作为百度路径的兜底。
#
# 用法（在云主机源码树上）：
#   bash /root/openvela-build/scripts/fix_mic_route_scope.sh

set -u

AI=/root/openvela/packages/ai_agent
VC="$AI/src/voice/voice_channel.c"
MK="$AI/Makefile"

echo "=================================================================="
echo " 修复 mic_route 归属：板级职责改为后端无关"
echo "=================================================================="

[ -f "$VC" ] || { echo "[错误] 找不到 $VC"; exit 1; }

# ---------------------------------------------------------------- 1. 调用点
echo ""
echo "--- 1. 在 voice_channel_init() 里调用 mic_route_enable() ---"

if grep -q "mic_route_enable" "$VC"; then
  echo "  [跳过] 已经调用过 mic_route_enable()"
else
  cp "$VC" "$VC.microutebak"
  echo "  已备份 -> $VC.microutebak"

  # --- include：插在最后一个 #include "voice/ 之后 -------------------------
  LAST=$(grep -n '#include "voice/' "$VC" | tail -1 | cut -d: -f1)
  if [ -z "$LAST" ]; then
    LAST=$(grep -n '^#include' "$VC" | tail -1 | cut -d: -f1)
    echo "  [警告] 没找到 voice/ 的 include，改用最后一个 include（第 $LAST 行）"
  fi
  if [ -n "$LAST" ]; then
    awk -v n="$LAST" '
      { print }
      NR==n { print "#include \"voice/mic_route.h\"" }
    ' "$VC" > "$VC.tmp" && mv "$VC.tmp" "$VC"
    echo "  [OK] 第 $LAST 行后补 #include \"voice/mic_route.h\""
  else
    echo "  [错误] 找不到任何 include，请手工补头文件"
    exit 1
  fi

  # --- 调用：插在后端注册之前 --------------------------------------------
  # 用 s_backends_registered 作为锚点（register_baidu.sh 已确认该结构存在），
  # 这样无论后端表里最终注册了谁，通路都已经先打开。
  if grep -q "s_backends_registered" "$VC"; then
    awk '
      /if \(!s_backends_registered\)/ {
        print "    /* 麦克风采集通路：板级职责，与后端无关。"
        print "     * 原本只在 baidu_asr_init_ops() 里做，导致用火山后端时"
        print "     * 通路每次开机都是 Off、录音必败。详见《设计-闭环v2.md》决策 4。*/"
        print "    (void)mic_route_enable();"
      }
      { print }
    ' "$VC" > "$VC.tmp" && mv "$VC.tmp" "$VC"
    echo "  [OK] 已插入调用（在后端注册之前）"
  else
    echo "  [警告] 找不到 s_backends_registered 锚点"
    echo "         请手工在 voice_channel_init() 函数体开头加一行："
    echo "             mic_route_enable();"
    INIT_LINE=$(grep -n "voice_channel_init" "$VC" | head -3)
    echo "         参考位置：$INIT_LINE"
    exit 1
  fi
fi

# ---------------------------------------------------------------- 2. 构建登记
echo ""
echo "--- 2. 确保 mic_route.c 已登记进 Makefile ---"

if [ ! -f "$MK" ]; then
  echo "  [警告] 找不到 $MK，跳过登记检查"
elif grep -q "src/voice/mic_route.c" "$MK"; then
  echo "  [跳过] CSRCS 已含 src/voice/mic_route.c"
else
  cp -n "$MK" "$MK.microutebak" 2>/dev/null
  if grep -q "CSRCS += src/voice/volc_asr.c" "$MK"; then
    awk '
      { print }
      /CSRCS \+= src\/voice\/volc_asr\.c/ { print "CSRCS += src/voice/mic_route.c" }
    ' "$MK" > "$MK.tmp" && mv "$MK.tmp" "$MK"
    echo "  [OK] 已登记 CSRCS += src/voice/mic_route.c"
  else
    echo "  [警告] 没找到 volc_asr.c 登记行作为锚点，请手工加："
    echo "         CSRCS += src/voice/mic_route.c"
  fi
fi

# ---------------------------------------------------------------- 校验
echo ""
echo "--- 校验 ---"
grep -n "mic_route" "$VC" || echo "  (voice_channel.c 里没有 mic_route)"
echo ""
grep -n "mic_route" "$MK" 2>/dev/null || echo "  (Makefile 里没有 mic_route)"

echo ""
echo "=================================================================="
echo " 完成。重新编译："
echo "   cd /root/openvela-build && BOARD_CONFIG=configs/nsh_minidisplay \\"
echo "       bash scripts/remote_build_job.sh start"
echo ""
echo " 烧录后验证（应看到采集成功，且不再报 channel 错）："
echo "   adb shell \"amixer\"                     # 确认开关已为 On"
echo "   adb shell \"dmesg\" | grep -i snd        # 板上无 grep，改看完整 dmesg"
echo "   adb shell \"arecord -r 16000 -f 16 -c 1 -d 5 /data/r.wav\""
echo "   adb pull /data/r.wav                    # 应约 160,044 字节"
echo "=================================================================="
