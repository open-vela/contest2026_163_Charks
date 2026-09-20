#!/bin/sh
# 在云主机上把 R6 的配置改动应用到板级 defconfig
#
# 幂等：重复跑不会产生重复行。
# 每次改前先备份 defconfig -> defconfig.r6bak
#
# 用法：
#   bash /root/openvela-build/scripts/apply_r6_config.sh

set -u

D=/root/openvela/vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay
DEF="$D/defconfig"

[ -f "$DEF" ] || { echo "[错误] 找不到 $DEF"; exit 1; }

cp "$DEF" "$DEF.r6bak"
echo "已备份 -> $DEF.r6bak"
echo ""

# ---------------------------------------------------------------- 辅助函数
# set_kv KEY VALUE —— 存在则替换，不存在则追加
set_kv() {
    key="$1"; val="$2"
    if grep -q "^$key=" "$DEF" 2>/dev/null; then
        sed -i "s|^$key=.*|$key=$val|" "$DEF"
        echo "  [改] $key=$val"
    elif grep -q "^# $key is not set" "$DEF" 2>/dev/null; then
        sed -i "s|^# $key is not set|$key=$val|" "$DEF"
        echo "  [开] $key=$val   (原为 is not set)"
    else
        printf '%s=%s\n' "$key" "$val" >> "$DEF"
        echo "  [增] $key=$val"
    fi
}

enable_y() { set_kv "$1" "y"; }

# ---------------------------------------------------------------- 1. NTP
echo "--- 1. NTP 服务器换成国内（修 1970 时钟 -> HTTPS 证书校验）---"
echo "    依据：apps/netutils/ntpclient/Kconfig 里"
echo "          config NETUTILS_NTPCLIENT_SERVER, default \"0.pool.ntp.org;...\""
echo "          与板端实测看到的服务器列表完全一致"
echo "    注意：该 config depends on LIBC_NETDB，必须用域名不能用 IP"
set_kv CONFIG_NETUTILS_NTPCLIENT_SERVER '"ntp.aliyun.com;ntp1.aliyun.com"'
echo ""

# ---------------------------------------------------------------- 2. 必开项
echo "--- 2. 开启本项目需要的后端与依赖 ---"
enable_y CONFIG_NETUTILS_CJSON      # 官方 AI Agent 配置会关掉它，但百度后端依赖 cJSON
enable_y CONFIG_AI_AGENT_VOICE_BAIDU
enable_y CONFIG_AI_AGENT_CHILD_FACE
echo ""

# ---------------------------------------------------------------- 3. 报告
echo "--- 3. 校验结果 ---"
grep -E "NETUTILS_NTPCLIENT_SERVER|NETUTILS_CJSON|AI_AGENT_VOICE_BAIDU|AI_AGENT_CHILD_FACE" "$DEF"
echo ""

echo "--- 4. 百度后端注册点参考（需要你在 voice_channel_init 里加 1-2 行）---"
VC=/root/openvela/packages/ai_agent/src/voice/voice_channel.c
if [ -f "$VC" ]; then
    grep -n "register\|voice_channel_init" "$VC" | head -20
else
    echo "  （未找到 $VC）"
fi
echo ""

echo "--- 5. 显示相关的两种修法（需要你决定，脚本不改）---"
echo "  现状：/dev/fb0 存在，/dev/lcd0 不存在"
echo "        AI Agent defconfig 新增了 CONFIG_LCD_FRAMEBUFFER=y + LCD_EXTERNINIT=y"
echo "        而 luncher_mini(预编译) 与多个 demo 写死找 /dev/lcd0"
echo ""
echo "  方案 A（改配置）：关掉 CONFIG_LCD_FRAMEBUFFER -> 回到 /dev/lcd0"
echo "                    luncher_mini 立刻可用；风险：可能影响 AI Agent 显示栈"
echo "  方案 B（改代码）：保留 fb0，我们自己的 LVGL 入口传 /dev/fb0"
echo "                    （日志显示 lv_nuttx_lcd_create 的路径是参数传入的）"
echo ""
echo "  本轮先不改动显示配置 —— 其他三项修复与它独立，先让编译跑起来"
