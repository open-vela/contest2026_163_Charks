#!/bin/sh
# 在 voice_channel_init() 里注册百度 ASR/TTS 后端
#
# 目标位置（源码实勘）：
#   packages/ai_agent/src/voice/voice_channel.c
#     603:    if (!s_backends_registered) {
#     604:        volc_tts_register();
#     605:        volc_asr_register();
#     606:        s_backends_registered = 1;
#
# 做法：在 volc_asr_register() 之后插入 baidu_tts/register 与 baidu_asr/register，
#       并在 volc 的 #include 之后补上百度头文件。
#
# 幂等：已注册过就跳过。

set -u

VC=/root/openvela/packages/ai_agent/src/voice/voice_channel.c

[ -f "$VC" ] || { echo "[错误] 找不到 $VC"; exit 1; }

if grep -q "baidu_asr_register" "$VC" 2>/dev/null; then
    echo "  [跳过] 已经注册过了，不重复插入"
    exit 0
fi

cp "$VC" "$VC.r6bak"
echo "  已备份 -> $VC.r6bak"

# --- 1. 补 include：插在最后一个 volc 头文件 include 之后 -------------------
LAST_VOLC=$(grep -n '#include.*volc' "$VC" | tail -1 | cut -d: -f1)
if [ -n "$LAST_VOLC" ]; then
    awk -v n="$LAST_VOLC" '
        { print }
        NR==n {
            print "#include <voice/baidu_tts.h>"
            print "#include <voice/baidu_asr.h>"
        }
    ' "$VC" > "$VC.tmp" && mv "$VC.tmp" "$VC"
    echo "  [OK] 在第 $LAST_VOLC 行后补入百度头文件"
else
    echo "  [警告] 没找到 volc 的 include，头文件需手工补"
fi

# --- 2. 插注册调用：在 volc_asr_register(); 之后 ---------------------------
awk '
    { print }
    /volc_asr_register\(\);/ {
        print "        baidu_tts_register();"
        print "        baidu_asr_register();"
    }
' "$VC" > "$VC.tmp" && mv "$VC.tmp" "$VC"

echo "  [OK] 已插入 baidu_tts_register() / baidu_asr_register()"

echo ""
echo "--- 校验 ---"
grep -n "baidu" "$VC"
echo ""
echo "--- voice_channel_init 附近 ---"
grep -n -A6 "if (!s_backends_registered)" "$VC" | head -14
