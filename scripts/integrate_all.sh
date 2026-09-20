#!/bin/sh
# 把本项目的板端改动一次性集成进 openvela 源码树
#
# 目的
# ----
# 云主机是**按小时计费**的。手动敲十几条 cp / sed 很容易出错，
# 错了还要回头查 —— 那都是在烧钱。
# 本脚本把"集成"这件事压成一条命令，并且**幂等**（重复跑不会重复追加）。
#
# 用法
# ----
#   OPENVELA_SRC=~/openvela bash scripts/integrate_all.sh
#
# 它**不会**替你编译，也不会静默改配置里的关键项 ——
# 遇到"不确定"的地方（比如 NTP 的 config 名）只报告，让你自己确认。
# 这是刻意的：宁可多问一句，也不要写出一个看似成功、实际没生效的配置。

set -u

SRC="${OPENVELA_SRC:-$HOME/openvela}"
BOARD_DIR="$(cd "$(dirname "$0")/.." && pwd)/board"

AI_AGENT="$SRC/packages/ai_agent"
VOICE_INC="$AI_AGENT/include/voice"
VOICE_SRC="$AI_AGENT/src/voice"
UI_SRC="$AI_AGENT/src/ui"

ok()   { printf '  [OK]   %s\n' "$1"; }
warn() { printf '  [WARN] %s\n' "$1"; }
info() { printf '  [..]   %s\n' "$1"; }
die()  { printf '  [ERR]  %s\n' "$1"; exit 1; }

echo "=================================================================="
echo " 集成板端改动 -> $SRC"
echo "=================================================================="

# ---------------------------------------------------------------- 0. 前置检查
[ -d "$SRC" ] || die "源码树不存在: $SRC （用 OPENVELA_SRC= 指定）"
[ -d "$AI_AGENT" ] || die "找不到 packages/ai_agent：$AI_AGENT"

if [ -f "$VOICE_SRC/volc_asr.c" ]; then
    ok "确认这是 openvela AI Agent 源码树（找到 volc_asr.c）"
else
    warn "没找到 volc_asr.c —— 路径可能与你这版不同，下面的拷贝会失败"
fi

# ---------------------------------------------------------------- 1. 百度语音后端
echo ""
echo "--- 1. 百度语音后端 (board/baidu_voice) ---"

mkdir -p "$VOICE_INC" "$VOICE_SRC" || die "无法创建目标目录"

for f in baidu_auth.h baidu_asr.h baidu_tts.h mic_route.h; do
    if [ -f "$BOARD_DIR/baidu_voice/$f" ]; then
        cp "$BOARD_DIR/baidu_voice/$f" "$VOICE_INC/" && ok "include/voice/$f"
    else
        warn "缺少 $BOARD_DIR/baidu_voice/$f"
    fi
done

for f in baidu_auth.c baidu_asr.c baidu_tts.c mic_route.c; do
    if [ -f "$BOARD_DIR/baidu_voice/$f" ]; then
        cp "$BOARD_DIR/baidu_voice/$f" "$VOICE_SRC/" && ok "src/voice/$f"
    else
        warn "缺少 $BOARD_DIR/baidu_voice/$f"
    fi
done

# ---------------------------------------------------------------- 2. 表情界面
echo ""
echo "--- 2. 表情界面 (board/child_face) ---"

mkdir -p "$UI_SRC" || die "无法创建 $UI_SRC"

for f in child_face.h child_face.c; do
    if [ -f "$BOARD_DIR/child_face/$f" ]; then
        cp "$BOARD_DIR/child_face/$f" "$UI_SRC/" && ok "src/ui/$f"
    else
        warn "缺少 $BOARD_DIR/child_face/$f"
    fi
done

# ---------------------------------------------------------------- 3. Kconfig 片段
echo ""
echo "--- 3. Kconfig 片段（幂等追加）---"

append_once() {
    # $1 = 标记行, $2 = 片段文件, $3 = 目标 Kconfig
    marker="$1"; frag="$2"; target="$3"
    [ -f "$frag" ] || { warn "缺少 $frag"; return; }
    [ -f "$target" ] || { warn "缺少 $target"; return; }

    if grep -qF "$marker" "$target" 2>/dev/null; then
        ok "已存在，跳过: $target ($marker)"
    else
        printf '\n%s\n' "$marker" >> "$target"
        cat "$frag" >> "$target"
        ok "已追加: $target"
    fi
}

append_once "# >>> board/baidu_voice" "$BOARD_DIR/baidu_voice/Kconfig.fragment" \
            "$AI_AGENT/Kconfig"
append_once "# >>> board/child_face" "$BOARD_DIR/child_face/Kconfig.fragment" \
            "$AI_AGENT/Kconfig"

# ---------------------------------------------------------------- 4. NTP（只报告，不改）
echo ""
echo "--- 4. NTP 服务器（需要你确认，脚本不会自动改）---"

NTP_HITS=$(grep -rlE 'NTPCLIENT_SERVER|SYSTEM_NTPC_SERVER' \
           "$SRC"/nuttx "$SRC"/apps "$SRC"/external 2>/dev/null \
           | grep -i 'Kconfig' | head -5)

if [ -n "$NTP_HITS" ]; then
    ok "找到候选 config（请挑一个）："
    for h in $NTP_HITS; do
        printf '      %s\n' "$h"
        grep -hoE 'config [A-Z_]*NTP[A-Z_]*|CONFIG_[A-Z_]*NTP[A-Z_]*' "$h" 2>/dev/null \
            | sort -u | head -4 | sed 's/^/        /'
    done
    echo ""
    warn "请把板级 defconfig 里的对应项改成 ntp.aliyun.com（国内可达）"
else
    warn "没找到 NTP 的 Kconfig —— 可能源码树不完整，或名字不匹配。"
    info "手动找： grep -rE 'NTPCLIENT_SERVER|SYSTEM_NTPC_SERVER' $SRC --include=Kconfig"
fi

# ---------------------------------------------------------------- 5. 后续待办提醒
echo ""
echo "=================================================================="
echo " 还需要你手动确认的三件事（脚本不代劳）"
echo "=================================================================="
cat <<'EOM'
  1) defconfig 里开启：
       CONFIG_AI_AGENT_VOICE_BAIDU=y
       CONFIG_AI_AGENT_CHILD_FACE=y
       CONFIG_NETUTILS_CJSON=y      <-- 官方 AI Agent 配置会关掉它，必须补回！
     （前两项可用 prepare_r5_config.sh 之后手工追加，或直接 menuconfig 选）

  2) 在 voice_channel_init() 里加注册调用（1-2 行）：
       baidu_asr_register();
       baidu_tts_register();
     具体位置见 board/baidu_voice/README.md 第 3.4 节。
     这一步依赖 voice_channel.c 的实际写法，脚本不自动改。

  3) 屏幕：/dev/lcd0 在当前 R5 固件里不存在（LVGL 初始化失败）。
     需要从 R2 的 nsh_minidisplay defconfig 里 diff 出显示相关项补回来：
       diff a_defconfig b_defconfig | grep -iE "lcd|disp|fb|panel|ili|spi|backlight"
EOM

echo ""
echo "集成步骤完成。接下来跑："
echo "  cd \$(dirname \$0)/.. && BOARD_CONFIG=configs/nsh_minidisplay \\"
echo "      bash scripts/remote_build_job.sh start"
