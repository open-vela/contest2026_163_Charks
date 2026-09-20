#!/usr/bin/env bash
#
# inspect_usb_net.sh —— 调查「用 USB 网络共享绕过 WiFi」的可行性。
#
# 背景：板子 WiFi 芯片是 D 版本，而厂商镜像只有 A/B 版本固件，芯片拒绝加载
# （download firmware FAIL! status=0x27），所以 WiFi 不可用。
# R5 的云调用需要网络 —— 改走 USB 网络共享：
#   板子作为 USB 网络设备（RNDIS）→ Windows 原生识别 → 开 ICS 共享上网。
#
# NuttX 侧有三种 USB 网络类驱动（都在 nuttx/drivers/usbdev/ 下）：
#   rndis.c     RNDIS  —— **Windows 原生支持，不用装驱动** ← 首选
#   cdcecm.c    CDC-ECM —— Linux 友好，Windows 需装驱动
#   cdcncm.c    CDC-NCM
#
# 本脚本只做只读调查，不改任何东西。

set -uo pipefail

SRC="${OPENVELA_SRC:-/root/openvela}"
NX="$SRC/nuttx"

hr() { echo "--------------------------------------------------------------"; }

echo "=============================================================="
echo "  USB 网络共享可行性调查"
echo "=============================================================="
echo

# ---------------------------------------------------------------------------
echo "[1] usbdev 目录下的 Kconfig 在哪"
hr
ls -1 "$NX/drivers/usbdev/" 2>/dev/null | grep -i kconfig || echo "  (usbdev 下没有 Kconfig)"
echo
echo "  drivers/Kconfig 里与 USBDEV 相关的 source 行："
grep -n "usbdev" "$NX/drivers/Kconfig" 2>/dev/null | head -10 || echo "  (无)"

# ---------------------------------------------------------------------------
echo
echo "[2] RNDIS / CDCECM / NCM 的 Kconfig 定义"
hr
for sym in USBDEV_RNDIS NET_RNDIS USBDEV_CDCECM NET_CDCECM USBDEV_CDC_NCM NET_CDC_NCM; do
    hits=$(grep -rn "config $sym" "$NX/drivers" 2>/dev/null | head -3)
    if [ -n "$hits" ]; then
        echo "  FOUND  $sym"
        echo "$hits" | sed 's/^/         /'
    else
        echo "  --     $sym"
    fi
done

# ---------------------------------------------------------------------------
echo
echo "[3] RNDIS 驱动的对外接口（板级需要调什么）"
hr
grep -n "rndis_initialize\|struct rndis\|netdev" "$NX/include/nuttx/usb/rndis.h" 2>/dev/null | head -20

# ---------------------------------------------------------------------------
echo
echo "[4] 板子当前 USB 配置（.config）"
hr
CFG="$NX/.config"
if [ -f "$CFG" ]; then
    grep -E "^CONFIG_USBDEV|^CONFIG_USB_DEVICE|^CONFIG_USBADB|^CONFIG_USB_GADGET|^CONFIG_USBDEV_COMPOSITE|^CONFIG_NET_CDC|^CONFIG_NET_RNDIS|^CONFIG_USBDEV_BOARD" "$CFG" | sed 's/^/  /'
    echo
    echo "  ADB / composite 相关是否为 y："
    grep -cE "^CONFIG_USBADB=y" "$CFG" | sed 's/^/    USBADB=y 命中 /'
    grep -cE "^CONFIG_USBDEV_COMPOSITE=y" "$CFG" | sed 's/^/    COMPOSITE=y 命中 /'
else
    echo "  找不到 $CFG"
fi

# ---------------------------------------------------------------------------
echo
echo "[5] R528 的 USB 设备控制器代码"
hr
find "$SRC/vendor/allwinnertech" -type d -name "*usb*" 2>/dev/null | head -10
echo
echo "  R528 芯片侧 USB device 驱动："
find "$SRC" -path "*r528*" -name "*udc*" -o -path "*sun20iw1*" -name "*usb*" 2>/dev/null | head -10

# ---------------------------------------------------------------------------
echo
echo "[6] 板级 USB 初始化（当前 ADB 是怎么起来的）"
hr
BOARD_SRC="$SRC/vendor/allwinnertech/boards/r528/r528s3-gemini-s1/src"
grep -rn "usbdev\|adb\|usb_" "$BOARD_SRC"/*.c 2>/dev/null | head -15

# ---------------------------------------------------------------------------
echo
echo "[7] 上游是否有现成的 RNDIS 板级范例可抄"
hr
grep -rln "rndis_initialize" "$SRC" 2>/dev/null | head -10

# ---------------------------------------------------------------------------
echo
echo "[8] 上游是否有现成的 ECM/NCM 板级范例"
hr
grep -rln "cdcecm_initialize\|cdcncm_initialize" "$SRC" 2>/dev/null | head -10

# ---------------------------------------------------------------------------
echo
echo "[9] 网络栈基础（USB 网络需要这些）"
hr
if [ -f "$CFG" ]; then
    for k in CONFIG_NET CONFIG_NET_TCP CONFIG_NET_ICMP CONFIG_NET_ROUTE CONFIG_NET_ARP \
             CONFIG_NETUTILS_DHCPC CONFIG_NETUTILS_DHCPD CONFIG_NET_SOCKOPTS; do
        v=$(grep -E "^${k}=y" "$CFG" 2>/dev/null | head -1)
        if [ -n "$v" ]; then echo "  OK   $k"; else echo "  --   $k"; fi
    done
fi

echo
echo "=== DONE ==="
