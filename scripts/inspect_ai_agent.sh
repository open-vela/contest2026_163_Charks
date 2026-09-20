#!/usr/bin/env bash
#
# inspect_ai_agent.sh —— 对比官方 AI Agent defconfig 与板子现有配置，得出**精确增量**。
#
# 背景
# ----
# packages/ai_agent/defconfigs/gemini-s1/gemini-s1_defconfig 是官方为本板准备的
# "带 AI Agent 的完整板级配置"（612 行）。它本质上是
#   vendor/.../configs/nsh_minidisplay/defconfig  +  AI Agent 相关开关
#
# 所以不要靠猜着往 defconfig 里加 CONFIG_XXX=y —— 直接把两份做 diff，
# 拿到确切的"要加什么、要去掉什么"。
#
# 用法（远端）：
#     bash /root/openvela-build/scripts/inspect_ai_agent.sh

set -uo pipefail

SRC="${OPENVELA_SRC:-/root/openvela}"

OFFICIAL="$SRC/packages/ai_agent/defconfigs/gemini-s1/gemini-s1_defconfig"
BOARD="$SRC/vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay/defconfig"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

for f in "$OFFICIAL" "$BOARD"; do
    if [[ ! -f "$f" ]]; then
        echo "ERROR: not found: $f" >&2
        exit 1
    fi
done

# 只比较**生效的**配置行（去掉注释与空行），排序去重后对比
grep -v '^#' "$OFFICIAL" | grep -v '^[[:space:]]*$' | sort -u > "$TMP/off"
grep -v '^#' "$BOARD"    | grep -v '^[[:space:]]*$' | sort -u > "$TMP/brd"

echo "=============================================================="
echo "  AI Agent 配置增量分析"
echo "=============================================================="
echo
echo "官方配置 : $OFFICIAL"
echo "板子配置 : $BOARD"
echo
echo "行数：官方 $(wc -l < "$OFFICIAL") / 板子 $(wc -l < "$BOARD")"
echo "生效项：官方 $(wc -l < "$TMP/off") / 板子 $(wc -l < "$TMP/brd")"
echo

echo "--------------------------------------------------------------"
echo " [A] 只在官方配置里 -> 要**加进**板级 defconfig"
echo "--------------------------------------------------------------"
comm -23 "$TMP/off" "$TMP/brd"

echo
echo "--------------------------------------------------------------"
echo " [B] 只在板子配置里 -> 官方把它**关掉了**（多为裁剪，需逐条判断）"
echo "--------------------------------------------------------------"
comm -13 "$TMP/off" "$TMP/brd"

echo
echo "--------------------------------------------------------------"
echo " [C] 官方配置里的 AI Agent / MEDIA / FFMPEG 相关项（完整清单）"
echo "--------------------------------------------------------------"
grep -E '^CONFIG_(AI_AGENT|EXAMPLES_AI_AGENT|MEDIA|LIB_FFMPEG)' "$OFFICIAL" || echo "  (none)"

echo
echo "--------------------------------------------------------------"
echo " [D] 板子现有配置里的对应项（看当前值）"
echo "--------------------------------------------------------------"
grep -E '^CONFIG_(AI_AGENT|EXAMPLES_AI_AGENT|MEDIA|LIB_FFMPEG)' "$BOARD" || echo "  (none - 说明这些功能当前全关)"

echo
echo "--------------------------------------------------------------"
echo " [E] ai_agent 的 Kconfig 顶层菜单项（找主开关真名）"
echo "--------------------------------------------------------------"
grep -nE 'config |menuconfig ' "$SRC/packages/ai_agent/Kconfig" 2>/dev/null | head -30

echo
echo "--------------------------------------------------------------"
echo " [F] defconfigs 目录里的 README（官方说明）"
echo "--------------------------------------------------------------"
README="$SRC/packages/ai_agent/defconfigs/gemini-s1/README.md"
if [[ -f "$README" ]]; then
    sed -n '1,60p' "$README"
else
    echo "  (no README)"
fi

echo
echo "=== DONE ==="
