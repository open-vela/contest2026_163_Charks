#!/usr/bin/env bash
# ============================================================================
# 把 board/superchild/ 集成进 openvela 源码树，并做好"开机自启"的挂接。
#
# 幂等：重复跑只会覆盖代码、补齐缺失的挂接点。
#
# 用法（在源码树所在机器上）：
#     OPENVELA_SRC=/root/openvela bash scripts/integrate_superchild.sh
#
# 它做五件事：
#   1. 拷贝应用代码 → packages/demos/superchild/（复用 demos 的自动纳入机制）
#   2. 板级 defconfig 打开 CONFIG_SUPERCHILD=y 与子项
#   3. 打一个"开机自启"的补丁：board_app_initialize() 里创建 superchild 任务
#   4. 校验：把关键项与挂接点逐条打出来给你看
#   5. 报告还需要人工确认的项
# ============================================================================

set -uo pipefail

SRC="${OPENVELA_SRC:-/root/openvela}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
BOARD_SRC="$HERE/board/superchild"
BOARD_DIR="$SRC/vendor/allwinnertech/boards/r528/r528s3-gemini-s1"
DEFCONFIG="$BOARD_DIR/configs/nsh_minidisplay/defconfig"
DEST="$SRC/packages/demos/superchild"
APPINIT="$BOARD_DIR/src/r528_appinit.c"

ok()   { echo "  [OK]   $*"; }
warn() { echo "  [WARN] $*"; }
info() { echo "  [..]   $*"; }
die()  { echo "  [ERR]  $*" >&2; exit 1; }

echo "=================================================================="
echo " 集成 superchild → $SRC"
echo "=================================================================="

[[ -d "$SRC" ]] || die "源码树不存在：$SRC（用 OPENVELA_SRC= 指定）"
[[ -d "$BOARD_SRC" ]] || die "找不到 $BOARD_SRC"
[[ -f "$DEFCONFIG" ]] || die "找不到板级配置：$DEFCONFIG"
[[ -f "$APPINIT" ]] || die "找不到 $APPINIT"

# ---------------------------------------------------------------- 0. 前置
echo
echo "--- 0. 前置检查 ---"
[ -d "$SRC/packages/demos" ] || die "找不到 packages/demos —— 源码树结构不对"
grep -q 'packages/demos/\*/Make.defs' "$SRC/packages/demos/Make.defs" 2>/dev/null \
    && ok "确认 demos 有自动纳入机制（wildcard Make.defs）" \
    || warn "packages/demos/Make.defs 里没找到 wildcard —— 可能收不进去，请人工确认"

if [ -f "$SRC/packages/ai_agent/Makefile" ]; then
    ok "参照 packages/ai_agent 的 include 路径写法（aw-alsa-lib）"
else
    warn "没找到 packages/ai_agent —— include 路径请自行确认"
fi

# ---------------------------------------------------------------- 1. 拷贝
echo
echo "--- 1. 拷贝应用代码 ---"
mkdir -p "$DEST" || die "创建 $DEST 失败"

# ⚠️ 先清掉源码树里已有的 .c/.h。
#
# 不清的话，**被删除的源文件会留在树里继续参与编译** ——
# 方案 C 把 sc_net.c / sc_net.h 删掉了，但树里那份还在，
# 于是 make 会去编一个引用了已删除头文件的 .c，
# 报一堆"找不到 xxx"的错误，看起来像"新代码写错了"。
# 这个坑在改动文件集合时一定会遇到，所以固化在这里。
if [ -d "$DEST" ]; then
    rm -f "$DEST"/*.c "$DEST"/*.h
    ok "已清空 $DEST 里的旧 .c/.h（防止已删除的源文件残留参与编译）"
fi

n=0
for f in "$BOARD_SRC"/*.c "$BOARD_SRC"/*.h "$BOARD_SRC"/Kconfig \
         "$BOARD_SRC"/Makefile "$BOARD_SRC"/Make.defs; do
    [ -f "$f" ] || continue
    cp "$f" "$DEST/" && n=$((n + 1))
done
ok "拷入 $n 个文件 → packages/demos/superchild/"
ls -1 "$DEST" | sed 's/^/         /'

# ---------------------------------------------------------------- 2. 配置
echo
echo "--- 2. 板级 defconfig ---"
set_kv() {
    key="$1"; val="$2"
    if grep -q "^$key=" "$DEFCONFIG" 2>/dev/null; then
        sed -i "s|^$key=.*|$key=$val|" "$DEFCONFIG"; echo "  [改] $key=$val"
    elif grep -q "^# $key is not set" "$DEFCONFIG" 2>/dev/null; then
        sed -i "s|^# $key is not set|$key=$val|" "$DEFCONFIG"; echo "  [开] $key=$val"
    else
        printf '%s=%s\n' "$key" "$val" >> "$DEFCONFIG"; echo "  [增] $key=$val"
    fi
}

# 删掉一行（用于清掉厂商写死的、会打架的选择项）
del_kv() {
    key="$1"
    if grep -q "^$key=\|^# $key is not set" "$DEFCONFIG" 2>/dev/null; then
        sed -i "/^$key=\|^# $key is not set/d" "$DEFCONFIG"
        echo "  [删] $key"
    fi
}

set_kv CONFIG_SUPERCHILD y
set_kv CONFIG_SUPERCHILD_AUTOSTART y
set_kv CONFIG_SUPERCHILD_WIFI_GUARD y

# ---- 方案 C：直连 StepFun 的四个参数 ----
#
# HOST 是**域名**，承担 SNI + 证书主体匹配；IP 是 TCP 实际连的地址。
# 两者不能混：把域名填进 IP 会连不上（板子上不解析域名），
# 把 IP 填进 HOST 会让证书校验失败，报错看起来像"证书不对"。
set_kv CONFIG_SUPERCHILD_API_HOST '"api.stepfun.com"'
set_kv CONFIG_SUPERCHILD_API_IP '"14.103.2.83"'
set_kv CONFIG_SUPERCHILD_API_PORT 443
set_kv CONFIG_SUPERCHILD_MODEL '"stepaudio-3-realtime-preview"'
# 服务端默认音色是 jingdiannvsheng，必须显式指定才是我们要的
set_kv CONFIG_SUPERCHILD_VOICE '"linjiajiejie"'
# TLS 握手 + cJSON 解析的栈深度要求
set_kv CONFIG_SUPERCHILD_RT_STACKSIZE 20480

# ---- TLS 需要的三个内核侧依赖 ----
#
# CRYPTO_MBEDTLS / NETUTILS_CJSON 由 superchild 的 Kconfig `select` 带出来，
# 但 NTP 客户端**不会**被自动带上，而它是证书有效期校验的前提：
# rcS 里的 `ntpcstart &` 只有在 CONFIG_NETUTILS_NTPCLIENT=y 时才真的存在。
# 少了它，证书校验会失败并报 `Certificate validity failed` ——
# 看起来像"证书过期"，实际是"系统时间还是 1970"。
set_kv CONFIG_NETUTILS_NTPCLIENT y

# ⚠️ 这里踩过一个"静默失效"的坑，记下来：
#
#   `CONFIG_NETUTILS_NTPCLIENT_SERVERIP` 的 Kconfig 是
#       depends on !LIBC_NETDB
#   而板子**开了 LIBC_NETDB**（有 DNS），所以这一项**根本不存在** ——
#   往里写的 IP 被 kconfig 静默丢掉，一点提示都没有。
#   实际生效的是 CONFIG_NETUTILS_NTPCLIENT_SERVER（主机名列表，分号隔开），
#   默认值 `0.pool.ntp.org;1.pool.ntp.org;2.pool.ntp.org`
#   **在国内基本连不上或极慢**，于是 ntpcstatus 永远显示 0 个样本。
#
#   而且这台板子上**校园网还封了出站 UDP 123** —— 就算换了正确的服务器
#   也没用。所以真正可用的时间同步在应用侧（sc_time.c，走 HTTP Date
#   的 TCP:80）。这里配好只是为了让系统自带的 ntpc 在别的网络下也能工作。
del_kv CONFIG_NETUTILS_NTPCLIENT_SERVERIP

# ⚠️ 下面这行**暂时不改**：改字符串配置会让 nuttx/include/nuttx/config.h
#    变化，触发**全量重编（40 分钟）**。而系统自带的 ntpc 在这台板子上
#    本来就用不了（UDP 123 被封），真正干活的是应用侧的 sc_time.c。
#    所以要验证时间同步这条路径时，先保持增量编译（几分钟）。
#    等别的改动也要动 config.h 时，再一起把它打开。
# set_kv CONFIG_NETUTILS_NTPCLIENT_SERVER '"203.107.6.88;120.25.115.20"'

# mbedtls 的熵源走 getrandom()/dev-urandom。两者至少要有一个可用，
# 否则 mbedtls_ctr_drbg_seed 会失败，现象是"TLS 初始化失败"。
set_kv CONFIG_DEV_URANDOM y
set_kv CONFIG_CRYPTO_RANDOM_POOL y

# ---------------------------------------------------------------- 2b. WiFi
#
# 板子的 rcS.nsh 里虽然有开机联网脚本，但那条链是断的：
#   · /data/etc/wifi/wapi.conf 不在打包素材里 → `if [ -f ... ]` 为假，不跑
#   · 就算跑，start_wifi.sh 用 grep + $(...) 取值，而板上两者都没有
# 真正可用的是官方 netinit 的编译期配置。这里把 SSID/PASSPHRASE
# 同步给内核的那两个 config。
#
# ⚠️ 用环境变量传入，**不要写进 defconfig**（那会进版本库）。
#    密码是私密信息，写在命令行历史里也比写在仓库里好。
echo ""
echo "--- 2b. WiFi（开机自动联网的关键一环）---"

# ---- 加密方式：必须从 TKIP 改成 CCMP ----------------------------------------
#
# 2026-09-20 发现：板级 defconfig 把 WPA 加密方式**显式覆盖成了 TKIP**：
#     CONFIG_NETINIT_WAPI_CIPHERMODE_TKIP=y   → 0x04
#     CONFIG_NETINIT_WAPI_ALG_TKIP=y          → 2
# 而 nnetinit 的 Kconfig 默认值是 CCMP（WPA2-AES），厂商自己那份能用的
# wapi.conf 样例也是 `"auth":4,"cmode":8,"alg":3` = WPA2 + AES-CCMP。
#
# 现代路由器绝大多数是 **WPA2-AES 专用**，只支持 TKIP 的客户端会关联失败，
# 而且失败现象很模糊（`No Assoc Network After Scan Done`），
# 看起来像信号问题，很容易往错的方向排查。
#
# 做法：**删掉这几行，让 Kconfig 用它自己的默认值（CCMP + WPA2）**。
# 比手写一堆 choice 的 hex 值可靠 —— choice 的派生值由 kconfig 自己算。
del_kv CONFIG_NETINIT_WAPI_CIPHERMODE_TKIP
del_kv CONFIG_NETINIT_WAPI_ALG_TKIP
del_kv CONFIG_NETINIT_WAPI_AUTHWPA_WPA_WPA2
# 同时清掉派生的数值行，交给 kconfig 从 choice 重算
del_kv CONFIG_NETINIT_WAPI_CIPHERMODE
del_kv CONFIG_NETINIT_WAPI_ALG
del_kv CONFIG_NETINIT_WAPI_AUTHWPA
info "WPA 加密方式已交还给 Kconfig 默认值（WPA2 + AES-CCMP）"

# ---------------------------------------------------------------- 2c. 网络
#
# ⚠️⚠️ 这一节是 2026-09-20 **上板实测踩坑之后**补的，三个坑叠在一起
#      导致"WiFi 明明能连、但板子一直连不上服务器"。逐一说明：
#
# 【坑 1】打包素材里有一份写死 BSSID 的 wapi.conf
#
#   lichee/board/common/data/UDISK/etc/wifi/wapi.conf 内容：
#       {"wlan0": {"ssid": "Rivotek-Visitor",
#                  "bssid": "be:c1:a6:ff:d6:5e",
#                  "psk": "Rxw@2511", "auth": 4, "cmode": 8, "alg": 3}}
#
#   它是**厂商自己测试用的**，被打进 UDISK 分区 —— 也就是**每次刷机都会回来**。
#   而 netinit_associate.c 的逻辑是：
#       load = wapi_load_config(...);      // 读到了就用它
#       if (!load) { 用 CONFIG_NETINIT_WAPI_SSID }
#   于是我们配的 SSID 永远不生效，驱动一直去连 Rivotek-Visitor，
#   而它的 BSSID 是写死的 → 驱动报
#       RTL871X: rtl_select_and_join_from_scanned_queue: return _FAIL(candidate == NULL)
#       RTL871X: No Assoc Network After Scan Done
#   **"扫到了但连不上"** —— 看起来像信号问题或密码问题，实际是配置文件把
#   网卡钉死在了另一个 AP 上。
#
#   注意这份文件的路径正是本项目文档里记过的"换网络必死"的根因，
#   但当时以为它不在打包素材里 —— 判断错了。
#
# 【坑 2】netinit 用了静态 IP，于是"有没有 IP"永远为真
#
#   CONFIG_NETINIT_IPADDR=0x0a000002 → 10.0.0.2
#   CONFIG_NETINIT_DRIPADDR=0x0a000001 → 10.0.0.1
#   不管有没有关联上 AP，wlan0 永远"有 IP"。这直接让
#   sc_wifi.c 的检查失效（见那个文件里的长注释）。
#   而且这个网段是厂商按他们自己的测试 AP 设的，换到别的网络必然不通。
#
# 【坑 3】DHCP 库其实开着，只是 netinit 没用它
#
#   CONFIG_NETUTILS_DHCPC=y 本来就有，缺的只是 CONFIG_NETINIT_DHCPC。
#   开了它之后，板子在任何网络下都能拿到正确的 IP 和网关 ——
#   **这才是可移植的做法**（换个 WiFi 不用重编固件）。
echo ""
echo "--- 2c. 网络（清掉厂商写死的 WiFi 配置，改用 DHCP）---"

# 坑 1：把那份写死 BSSID 的配置从**打包素材**里删掉。
# 只删板子上的没用 —— 重刷一次就回来了。
WAPI_CONF="$SRC/vendor/allwinnertech/lichee/board/common/data/UDISK/etc/wifi/wapi.conf"
if [ -f "$WAPI_CONF" ]; then
    # 留一份副本在**打包目录之外**，万一将来要做对照测试还能找回来。
    # ⚠️ 不能就地留成 .bak：那个目录里的任何文件都会被一起打进镜像，
    #    镜像里多一个无意义的小文件虽然无害，但会让人以为
    #    "这个配置文件还在起作用"。
    cp "$WAPI_CONF" /root/wapi.conf.vendor-original 2>/dev/null
    rm -f "$WAPI_CONF"
    warn "已从打包素材里删除写死 BSSID 的 wapi.conf（副本在 /root/）"
    warn "  它原本会覆盖 CONFIG_NETINIT_WAPI_SSID，导致驱动一直去连"
    warn "  Rivotek-Visitor / be:c1:a6:ff:d6:5e，报『No Assoc Network After Scan Done』"
else
    info "打包素材里没有 wapi.conf（已是干净状态）"
fi

# ⚠️ 还有一个**编译期修不掉**的地方要记住：
#    如果这次没有全盘擦除 /data，板子上原来那份
#    /data/etc/wifi/wapi.conf 会**继续存在并继续生效**（它优先于编译期配置）。
#    刷完机如果还是连不上，先在板子上执行：
#        mv /data/etc/wifi/wapi.conf /data/etc/wifi/wapi.conf.bak
#    然后重启。

# 坑 2 + 3：改用 DHCP
set_kv CONFIG_NETINIT_DHCPC y
del_kv CONFIG_NETINIT_IPADDR
del_kv CONFIG_NETINIT_DRIPADDR
del_kv CONFIG_NETINIT_NETMASK
info "已改为 DHCP 获取地址（删掉厂商的静态 IP 10.0.0.2/24）"

# DHCP 客户端的行为参数：重试次数给足，因为开机时 AP 可能还没起来
set_kv CONFIG_NETUTILS_DHCPC_RETRIES 20
set_kv CONFIG_NETUTILS_DHCPC_RECV_TIMEOUT_MS 400
if [ -n "${WIFI_SSID:-}" ]; then
    set_kv CONFIG_SUPERCHILD_WIFI_SSID "\"$WIFI_SSID\""
    set_kv CONFIG_NETINIT_WAPI_SSID "\"$WIFI_SSID\""
    if [ -n "${WIFI_PASS:-}" ]; then
        set_kv CONFIG_SUPERCHILD_WIFI_PASSPHRASE "\"$WIFI_PASS\""
        set_kv CONFIG_NETINIT_WAPI_PASSPHRASE "\"$WIFI_PASS\""
        ok "已写入 WiFi 凭据（SSID=$WIFI_SSID）"
    else
        warn "给了 WIFI_SSID 但没给 WIFI_PASS —— 开放网络可以，加密网络连不上"
    fi
else
    warn "没给 WIFI_SSID，固件里不带 WiFi 凭据。"
    warn "  开机自动联网会依赖以下之一，二选一："
    warn "    a) 重新跑：WIFI_SSID=你的WiFi WIFI_PASS=密码 bash $0"
    warn "    b) 烧完后 adb push 一份 /data/etc/wifi/wapi.conf"
    warn "       ⚠️ 那份配置里 bssid 是**必填**字段，但填了就钉死某个 AP，"
    warn "          换个网络必然连不上 —— 能留空就留空。"
fi

# mini_memo 与 superchild 会抢同一个 LVGL 实例和同一块屏。
# 两个都开的话先初始化成功的那个赢，另一个报 "LVGL already initialized"
# 然后退出 —— 结果取决于启动顺序，非常难查。这里直接关掉 mini_memo。
if grep -qE '^CONFIG_LVX_USE_DEMO_MINI_MEMO=y' "$DEFCONFIG"; then
    set_kv CONFIG_LVX_USE_DEMO_MINI_MEMO n
    info "已关 mini_memo（它会和 superchild 抢 LVGL）"
fi

# ---------------------------------------------------------------- 3. 自启
echo
echo "--- 3. 开机自启挂接 ---"
# 幂等判据必须看**任务创建**那一行，不能看 superchild_main ——
# include 块里也有这个名字，用它会误判成"已挂接"（第一版就踩了：
# include 写进去了、任务没建，再跑就说"已完成"）。
if grep -q 'task_create("superchild"' "$APPINIT" 2>/dev/null; then
    ok "已挂接过（$APPINIT 里有 task_create(\"superchild\")）"
else
    # 有备份就先还原，保证每次都是从"干净的厂商原文"开始改。
    # 否则上一次半途失败的修改会叠加进来，越改越乱。
    if [ -f "$APPINIT.scbak" ]; then
        cp "$APPINIT.scbak" "$APPINIT"
        info "已从 $APPINIT.scbak 还原，重新挂接"
    else
        cp "$APPINIT" "$APPINIT.scbak"
    fi

    # 选 board_app_initialize() 而不是 board_late_initialize()：
    # 后者在系统 bringup 阶段跑，那时显示/音频驱动未必就绪；
    # 前者由 nsh 通过 boardctl(BOARDIOC_INIT) 在驱动都注册完之后调用。
    python3 - "$APPINIT" <<'PYEOF'
import re, sys

path = sys.argv[1]
src = open(path, encoding="utf-8").read()

if 'task_create("superchild"' in src:
    print("  [跳过] 已经挂过了")
    raise SystemExit(0)

# ---------------------------------------------------------------------------
# 这里**故意不 include 任何新头文件**，只自己声明 task_create 的原型。
#
# 为什么（这是踩了一小时的坑，值得写清楚）
# ------------------------------------
# 第一版顺手加了 `#include <syslog.h>` 和 `#include <sched.h>`，结果编译报：
#     sys/select.h:87: 'OPEN_MAX' undeclared
#     nuttx/wdog.h:339: 'CLOCK_MAX' undeclared
# 报错点全在 NuttX 自己的头里，看起来像"NuttX 坏了"，
# 与真正的原因（我们在厂商板级文件里插了两个 include）毫无字面关系。
#
# 真因：`<limits.h>` 里的 OPEN_MAX / CLOCK_MAX 是**带条件**定义的，
# 依赖 `<nuttx/config.h>` 与其它头的先后关系。这个文件（厂商板级代码）
# 原本的包含链是"够用就好"的，多插一个 <syslog.h> 就会让 limits.h
# 在 config.h 生效之前先被读一遍，于是两个宏都没定义上。
#
# 结论：**在这种位置改代码，不要碰 include。**声明一个函数原型没有任何
# 副作用，是对厂商文件侵入最小的做法。
# ---------------------------------------------------------------------------
inc = (
    "\n"
    "#ifdef CONFIG_SUPERCHILD_AUTOSTART\n"
    "extern int superchild_main(int argc, FAR char *argv[]);\n"
    "/* 不 include <sched.h>，只声明原型 —— 理由见集成脚本里的长注释 */\n"
    "extern int task_create(const char *name, int priority,\n"
    "                       unsigned int stack_size,\n"
    "                       int (*entry)(int argc, char *argv[]),\n"
    "                       char *const argv[]);\n"
    "#endif\n"
)

incs = list(re.finditer(r"^#include[ \t].*$", src, re.M))
if incs:
    at = incs[-1].end()
    src = src[:at] + "\n" + inc + src[at:]
else:
    src = inc + src

# 找 board_app_initialize 的开头
m = re.search(r"^int\s+board_app_initialize\s*\([^)]*\)\s*\{", src, re.M)
if not m:
    print("  [ERR] 没找到 board_app_initialize，需要人工挂接")
    open(path, "w", encoding="utf-8").write(src)
    raise SystemExit(1)

# ⚠️ 插在**函数体开头**（`{` 之后），不是结尾。
#
# 第一版插在结尾，结果生成的是：
#     int board_app_initialize(uintptr_t arg)
#     {
#         return 0;                 ← 先返回了
#
#     #ifdef CONFIG_SUPERCHILD_AUTOSTART
#         task_create(...);          ← 死代码，永远不会执行
#     #endif
#     }
# 而且**编译和链接都不报错**（-Wno-error 把 unreachable 警告放过了），
# 固件烧进去之后表现为"开机什么都没发生"，非常难查。
# 两次构建的镜像大小一模一样，就是那时代码被编译器直接丢掉的证据。
at = m.end()

body = (
    "\n"
    "#ifdef CONFIG_SUPERCHILD_AUTOSTART\n"
    "  /* 开机直接进表情界面 —— 不需要敲命令、不需要 adb。\n"
    "   *\n"
    "   * 用 task_create 而不是在这里直接调 superchild_main()：\n"
    "   * 主任务是 UI 循环（永不返回），直接调会把 nsh 的启动流程卡死。\n"
    "   *\n"
    "   * ⚠️ 这段必须放在函数体**开头**。放结尾会被上面的 return 变成死代码。 */\n"
    "  (void)task_create(\"superchild\", CONFIG_SUPERCHILD_PRIORITY,\n"
    "                    CONFIG_SUPERCHILD_STACKSIZE, superchild_main, 0);\n"
    "#endif\n"
)

src = src[:at] + body + src[at:]
open(path, "w", encoding="utf-8").write(src)
print("  [OK] 已在 board_app_initialize() **开头**插入自启任务（未新增 include）")
PYEOF

    rc=$?
    if [ $rc -ne 0 ]; then
        warn "自动挂接失败，见 $APPINIT.scbak"
    fi
fi

# ---------------------------------------------------------------- 4. 校验
echo
echo "--- 4. 校验 ---"
echo "  配置项："
grep -E '^CONFIG_SUPERCHILD|^CONFIG_LVX_USE_DEMO_MINI_MEMO' "$DEFCONFIG" | sed 's/^/    /'
echo "  自启挂接点："
grep -n 'superchild_main\|SUPERCHILD_AUTOSTART' "$APPINIT" | sed 's/^/    /'
echo "  应用文件："
ls -1 "$DEST" | sed 's/^/    /'

OPEN=$(tr -cd '{' < "$APPINIT" | wc -c)
CLOSE=$(tr -cd '}' < "$APPINIT" | wc -c)
if [ "$OPEN" = "$CLOSE" ]; then
    ok "花括号配平（$OPEN）"
else
    warn "花括号不配平：{ =$OPEN, } =$CLOSE —— 请回滚 $APPINIT.scbak"
fi

cat <<'EOM'

==================================================================
 还需要人工确认的三件事
==================================================================
 1) **屏幕**：本应用假设 fb_path = /dev/lcd0（CONFIG_LV_USE_NUTTX_LCD=y）。
    如果你的屏幕方案是 /dev/fb0，改 sc_main.c 里那段
    `dsc.fb_path = ...` 或关掉 CONFIG_LV_USE_NUTTX_LCD。

 2) **aw-alsa-lib 的 include 路径**：Makefile 里写了三条 CFLAGS。
    若编译报 `aw-alsa-lib/pcm.h: No such file`，用下面这条找真实路径再改：
        find $SRC -name pcm.h -path '*aw-alsa-lib*'

 3) **API Key 要下发到板子**（方案 C 之后 Key **不在固件里**）：

        mkdir -p /data/etc/superchild
        echo <你的key> > /data/etc/superchild/stepfun.key

    adb 或 nsh 都行。**注意 `echo` 会带一个换行** ——
    不用担心，sc_key.c 会去掉首尾空白；但如果你是用别的方式写入，
    记得别把换行留在里面（带换行的 key 是鉴权失败，而报错看不出是换行）。

    Key 放 /data（YAFFS 可写分区，重启保留）而不是编进固件的理由：
    固件可以随便传、重刷、送人都不泄露密钥。
EOM

echo
echo "集成完成。下一步："
echo "  OPENVELA_SRC=$SRC BOARD_CONFIG=configs/nsh_minidisplay \\"
echo "      bash $HERE/scripts/build_openvela.sh"
