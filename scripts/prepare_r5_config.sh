#!/usr/bin/env bash
#
# prepare_r5_config.sh —— 把板级配置切换成「带 AI Agent」的版本（R5 前置）。
#
# 做什么
# ------
#   1. 备份现有的 nsh_minidisplay/defconfig
#   2. 装入官方 AI Agent defconfig（packages/ai_agent/defconfigs/gemini-s1/）
#   3. 把官方配置里**被关掉但我们需要**的项补回来（见下）
#   4. 运行官方 fix_gemini_s1.sh（最小音频框架 + PTT 录音补丁）
#
# 为什么要补 CONFIG_NETUTILS_CJSON
# -------------------------------
# 官方 gemini-s1_defconfig 是给 ai_agent + mini_memo 演示用的，它把
# CONFIG_NETUTILS_CJSON 关掉了。但本项目的**百度语音后端**要用 cJSON 解析
# 百度返回的 JSON（access_token、ASR result、TTS 错误码）。
# 而 packages/ai_agent 自身**不带** cJSON 副本（已确认 find 无结果）。
# 所以必须补回来，否则后端代码编不过。
#
# 幂等：重复执行结果一致。备份只在第一次做。
#
# 用法（远端）：
#     bash /root/openvela-build/scripts/prepare_r5_config.sh

set -uo pipefail

SRC="${OPENVELA_SRC:-/root/openvela}"

BOARD_CFG_DIR="$SRC/vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay"
BOARD_DEFCONFIG="$BOARD_CFG_DIR/defconfig"
AGENT_DEFCONFIG="$SRC/packages/ai_agent/defconfigs/gemini-s1/gemini-s1_defconfig"
FIX_SCRIPT="$SRC/packages/ai_agent/fix_gemini_s1.sh"

echo "=============================================================="
echo "  准备 R5 配置（AI Agent 版 nsh_minidisplay）"
echo "=============================================================="
echo

for f in "$BOARD_DEFCONFIG" "$AGENT_DEFCONFIG" "$FIX_SCRIPT"; do
    if [[ ! -f "$f" ]]; then
        echo "ERROR: not found: $f" >&2
        exit 1
    fi
done

# ---------------------------------------------------------------------------
# 1. 备份（只做一次）
# ---------------------------------------------------------------------------
echo "[1/4] 备份原配置"
if [[ -f "$BOARD_DEFCONFIG.orig" ]]; then
    echo "  .orig 已存在，跳过（保留最初那份）"
else
    cp "$BOARD_DEFCONFIG" "$BOARD_DEFCONFIG.orig"
    echo "  已备份 -> $(basename "$BOARD_DEFCONFIG").orig"
fi
echo "  原配置行数: $(wc -l < "$BOARD_DEFCONFIG.orig")"

# ---------------------------------------------------------------------------
# 2. 装入官方 AI Agent defconfig
# ---------------------------------------------------------------------------
echo
echo "[2/4] 装入官方 AI Agent defconfig"
cp "$AGENT_DEFCONFIG" "$BOARD_DEFCONFIG"
echo "  已装入，行数: $(wc -l < "$BOARD_DEFCONFIG")"

# ---------------------------------------------------------------------------
# 3. 补回被官方关掉、但本项目需要的项
# ---------------------------------------------------------------------------
echo
echo "[3/4] 补回本项目需要的配置项"

# 需要补回的清单：格式 "CONFIG_XXX=y|说明"
NEEDED=(
    "CONFIG_NETUTILS_CJSON=y|百度后端解析 JSON（access_token / ASR result / TTS 错误码）"
)

for entry in "${NEEDED[@]}"; do
    key="${entry%%|*}"
    why="${entry##*|}"

    if grep -qxF "$key" "$BOARD_DEFCONFIG"; then
        echo "  已存在，跳过: $key"
        continue
    fi

    # 如果被显式关掉（CONFIG_X is not set），先删掉那一行再追加，避免冲突
    sym="${key%%=*}"
    if grep -q "^# ${sym} is not set" "$BOARD_DEFCONFIG"; then
        sed -i "\|^# ${sym} is not set|d" "$BOARD_DEFCONFIG"
        echo "  已移除显式关闭项: # ${sym} is not set"
    fi

    printf '\n%s\n' "$key" >> "$BOARD_DEFCONFIG"
    echo "  已追加: $key"
    echo "     理由: $why"
done

# 顺便确认关键开关都在
echo
echo "  关键开关核对:"
for k in CONFIG_EXAMPLES_AI_AGENT_VELA CONFIG_MEDIA_SERVER CONFIG_LIB_FFMPEG \
         CONFIG_LIB_PFW CONFIG_NETUTILS_WEBCLIENT CONFIG_OPENSSL_MBEDTLS_WRAPPER \
         CONFIG_NETUTILS_CJSON; do
    if grep -qxF "${k}=y" "$BOARD_DEFCONFIG"; then
        echo "    OK   $k"
    else
        echo "    MISS $k"
    fi
done

# ---------------------------------------------------------------------------
# 4. 官方音频框架补丁
# ---------------------------------------------------------------------------
echo
echo "[4/4] 应用官方 fix_gemini_s1.sh"
cd "$SRC" || exit 1
if bash "$FIX_SCRIPT"; then
    echo "  fix_gemini_s1.sh 执行成功"
else
    echo "  WARN: fix_gemini_s1.sh 返回非零，请检查上面的输出"
fi

echo
echo "=============================================================="
echo "  完成。可以开始编译："
echo "    cd /root/openvela-build && FULL_REBUILD=1 \\"
echo "        BOARD_CONFIG=configs/nsh_minidisplay \\"
echo "        bash scripts/remote_build_job.sh start"
echo "=============================================================="
