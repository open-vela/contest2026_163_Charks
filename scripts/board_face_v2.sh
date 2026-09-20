#!/bin/sh
# 板端 v2：开机自动进入 face 界面 + 表情随语音状态变化 + 麦克风通路后端无关
#
# 依据（2026-09-20 在源码树上实勘）：
#   agent_main.c:588   voice_channel_init()      ← 开机 P3，**无条件执行**
#   agent_main.c:597   lvgl_ui_channel_init()    ← 开机 P3，但被
#                                                   #ifdef CONFIG_AI_AGENT_LVGL_UI 包着
#   而 packages/ai_agent/defconfigs/gemini-s1/gemini-s1_defconfig 里
#   **没有** CONFIG_AI_AGENT_LVGL_UI →
#   → lvgl_ui_channel_init() 被编译掉 → hook_child_face.py 注入的
#     child_face_init() **永远不会执行** → 这就是 face 界面不出现的根因。
#
# 本脚本做四件事（都幂等）：
#   1. 打开 CONFIG_AI_AGENT_LVGL_UI，让 lvgl_ui_channel_init() 真被执行
#   2. （可选）关掉 mini_memo，避免两个 UI 抢同一块屏
#   3. 把 mic_route_enable() 挂到 voice_channel_init()（后端无关）
#   4. 在语音状态机上挂表情切换：录制→聆听、停止中→思考、说话→说话、空闲→待机
#
# 用法：
#   bash scripts/board_face_v2.sh           # 全做（含关 mini_memo）
#   bash scripts/board_face_v2.sh --keep-memo

set -u

AI=/root/openvela/packages/ai_agent
AI_DEF="$AI/../ai_agent/defconfigs/gemini-s1/gemini-s1_defconfig"   # 占位，下面会重定位
VC="$AI/src/voice/voice_channel.c"
MK="$AI/Makefile"
BOARD=/root/openvela/vendor/allwinnertech/boards/r528/r528s3-gemini-s1
DEF="$BOARD/configs/nsh_minidisplay/defconfig"

KEEP_MEMO=0
[ "${1:-}" = "--keep-memo" ] && KEEP_MEMO=1

echo "=================================================================="
echo " 板端 v2：开机进 face + 表情随状态变化"
echo "=================================================================="
echo ""

# ---------------------------------------------------------------- 定位
# 官方 AI Agent defconfig 的位置有两种可能，都试一下
for c in "$AI/defconfigs/gemini-s1/gemini-s1_defconfig" \
         "/root/openvela/packages/ai_agent/defconfigs/gemini-s1/gemini-s1_defconfig"; do
    [ -f "$c" ] && AI_DEF="$c" && break
done

for f in "$VC" "$MK" "$DEF"; do
    [ -f "$f" ] || { echo "[错误] 找不到 $f"; exit 1; }
done
echo "源码树：$AI"
echo "板级配置：$DEF"
echo "AI Agent 配置：$AI_DEF"
echo ""

# ================================================================ 1. 配置
echo "--- 1. 打开 CONFIG_AI_AGENT_LVGL_UI（face 不出现的根因）---"
echo "    否则 lvgl_ui_channel_init() 被 #ifdef 编译掉，"
echo "    hook_child_face.py 注入的 child_face_init() 永远不执行。"

set_kv() {
    key="$1"; val="$2"; file="$3"
    if grep -q "^$key=" "$file" 2>/dev/null; then
        sed -i "s|^$key=.*|$key=$val|" "$file"; echo "  [改] $key=$val"
    elif grep -q "^# $key is not set" "$file" 2>/dev/null; then
        sed -i "s|^# $key is not set|$key=$val|" "$file"; echo "  [开] $key=$val（原为 is not set）"
    else
        printf '%s=%s\n' "$key" "$val" >> "$file"; echo "  [增] $key=$val"
    fi
}

# 板级 defconfig 才是最终生效的那份（build.sh 用它）
set_kv CONFIG_AI_AGENT_LVGL_UI y "$DEF"

if [ "$KEEP_MEMO" = "1" ]; then
    echo "  [保留] mini_memo 不动（--keep-memo）"
    echo "          ⚠ 两个 LVGL UI 同时抢 /dev/fb0，face 可能被盖住"
else
    set_kv CONFIG_LVX_USE_DEMO_MINI_MEMO n "$DEF"
    echo "     已关 mini_memo，把屏幕让给 child_face"
fi

# ================================================================ 2. 麦克风通路
echo ""
echo "--- 2. mic_route_enable() 挂到 voice_channel_init()（后端无关）---"

if grep -q "mic_route_enable" "$VC"; then
    echo "  [跳过] 已调用过"
else
    cp "$VC" "$VC.v2bak"

    LAST=$(grep -n '#include "voice/' "$VC" | tail -1 | cut -d: -f1)
    [ -z "$LAST" ] && LAST=$(grep -n '^#include' "$VC" | tail -1 | cut -d: -f1)
    if [ -n "$LAST" ]; then
        awk -v n="$LAST" '{print} NR==n{print "#include \"voice/mic_route.h\""}' \
            "$VC" > "$VC.tmp" && mv "$VC.tmp" "$VC"
        echo "  [OK] 第 $LAST 行后补 include"
    else
        echo "  [错误] 找不到 include 位置"; exit 1
    fi

    awk '
        /if \(!s_backends_registered\)/ {
            print "    /* 麦克风采集通路：板级职责，与后端无关。"
            print "     * 原本只在 baidu_asr_init_ops() 里做，导致用火山后端时"
            print "     * 通路每次开机都是 Off、录音必败。*/"
            print "    (void)mic_route_enable();"
        }
        {print}
    ' "$VC" > "$VC.tmp" && mv "$VC.tmp" "$VC"
    echo "  [OK] 已插入调用（在任何后端注册之前）"
fi

# ================================================================ 3. 表情状态钩子
echo ""
echo "--- 3. 语音状态 -> 表情 ---"

if grep -q "child_face_set_state" "$VC"; then
    echo "  [跳过] 表情钩子已存在"
else
    # include（幂等由上面的判断保证）
    LASTV=$(grep -n '#include "voice/' "$VC" | tail -1 | cut -d: -f1)
    awk -v n="$LASTV" '{print} NR==n{print "#include \"ui/child_face.h\""}' \
        "$VC" > "$VC.tmp" && mv "$VC.tmp" "$VC"

    hook() {
        # $1=匹配串 $2=要插入的调用
        awk -v pat="$1" -v call="$2" '
            $0 ~ pat { print; print call; next }
            {print}
        ' "$VC" > "$VC.tmp" && mv "$VC.tmp" "$VC"
    }

    # 开始录制 -> 聆听（眼睛睁大）
    hook '^    s_voice.state = VOICE_RECORDING;' \
         '    child_face_set_state(CHILD_FACE_LISTENING);'
    # 停止中（正在ASR/等模型）-> 思考
    hook '^    s_voice.state = VOICE_STOPPING;' \
         '    child_face_set_state(CHILD_FACE_THINKING);'
    # 说话开始 -> 说话（嘴开合）
    # 锚点选在 early-return 之后的那条 syslog：能确保真的要说才切表情，
    # 不会在"文本为空直接 return"时留下一个错误的表情。
    hook 'TAG, 60, clean, strlen\(clean\)\);' \
         '    child_face_set_state(CHILD_FACE_SPEAKING);'
    echo "  [OK] 挂载点：RECORDING→LISTENING / STOPPING→THINKING / speak→SPEAKING"
fi

# ================================================================ 4. 校验
echo ""
echo "--- 校验 ---"
echo "  voice_channel.c 里的新调用："
grep -n "mic_route_enable\|child_face_set_state\|child_face.h" "$VC" | sed 's/^/    /'
echo ""
echo "  板级配置："
grep -E "AI_AGENT_LVGL_UI|LVX_USE_DEMO_MINI_MEMO" "$DEF" | sed 's/^/    /'

# 括号配平粗查（防止上面 speak 钩子少补右括号）
OPEN=$(tr -cd '{' < "$VC" | wc -c)
CLOSE=$(tr -cd '}' < "$VC" | wc -c)
echo ""
echo "  花括号：{ = $OPEN, } = $CLOSE"
if [ "$OPEN" != "$CLOSE" ]; then
    echo "  [错误] 括号不配平（差 $((OPEN-CLOSE))）—— 脚本改坏了，请回滚 $VC.v2bak"
    exit 1
else
    echo "  [OK] 括号配平"
fi

echo ""
echo "=================================================================="
echo " 完成。重新编译："
echo "   cd /root/openvela-build && BOARD_CONFIG=configs/nsh_minidisplay \\"
echo "       bash scripts/remote_build_job.sh start"
echo ""
echo " 开机后应看到 face 界面（不再需要手动敲 ai_agent）。"
echo "=================================================================="
