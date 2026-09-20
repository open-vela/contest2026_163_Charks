#!/bin/bash
############################################################################
# scripts/install_pack_deps.sh
#
# 补齐打包（lichee pack）所需的宿主工具。
#
# 背景：固件编译链接都成功后，第 7 步打包会在 lichee 的 envsetup.sh 里报
#
#   tools/scripts/envsetup.sh: line 157:
#       pushd: /root/openvela/prebuilts/kconfig-frontends: No such file or directory
#
# 那一段的逻辑是：
#
#   if [ ! -f "${ROOTDIR}/prebuilts/kconfig-frontends/bin/kconfig-conf" ] &&
#      [ ! -x "$(command -v kconfig-conf)" ]; then
#       pushd ${ROOTDIR}/prebuilts/kconfig-frontends
#       ./configure --prefix=... && make install
#   fi
#
# 也就是：**只要系统里已有 kconfig-conf，就根本不会去碰那个目录**。
# openvela 的 manifest 里没有 kconfig-frontends 仓库，但它是 Ubuntu 的普通
# 软件包，直接装即可，比去补一个不存在的源码目录干净。
#
# 用法：bash scripts/install_pack_deps.sh
############################################################################

set -u

# 源码树位置（与 build_openvela.sh 保持一致，可用 OPENVELA_SRC 覆盖）。
# 只在末尾的 dragon 自检里用到，用来给出定位信息。
SRC_ROOT="${OPENVELA_SRC:-$HOME/openvela}"
SRC_ROOT_LICHEE="$SRC_ROOT/vendor/allwinnertech/lichee"

echo "=============================================================="
echo "  安装打包所需工具"
echo "=============================================================="

RC=0

# ---------------------------------------------------------------------------
# 1) kconfig-frontends —— lichee 的 envsetup.sh 会用到 kconfig-conf
# ---------------------------------------------------------------------------
echo
echo "[1/2] kconfig-conf"

if command -v kconfig-conf >/dev/null 2>&1; then
    echo "  已存在: $(command -v kconfig-conf)"
else
    for pkg in kconfig-frontends kconfig-frontends-nox; do
        echo "  尝试安装 $pkg …"
        if apt-get install -y "$pkg" >/dev/null 2>&1 && command -v kconfig-conf >/dev/null 2>&1; then
            echo "  安装成功: $(command -v kconfig-conf)"
            break
        fi
        echo "    $pkg 不可用"
    done

    if ! command -v kconfig-conf >/dev/null 2>&1; then
        echo "  [!] 没能装上 kconfig-conf。它只影响 lichee 打包，不影响固件本身。"
        RC=1
    fi
fi

# ---------------------------------------------------------------------------
# 2) 32 位运行库 —— 全志的 dragon 打包器是 i386 程序
# ---------------------------------------------------------------------------
#
# 这是最容易被误判的一处：pack 时报
#
#     tools/scripts/pack_img.sh: line 1001:
#         .../lichee/tools/tool/dragon: No such file or directory
#
# 但 dragon 文件**明明就在那里**，而且有可执行权限。真正的原因是它是
# 32 位 x86 程序（readelf 显示 Class: ELF32、Machine: Intel 80386），
# 而 64 位系统缺 i386 运行库时，execve 会返回 ENOENT —— shell 就把它
# 报成 "No such file or directory"，与"文件不存在"难以区分。
echo
echo "[2/2] 32 位运行库（dragon 是 i386 程序）"
echo

# ⚠️ 这里原来只判断了 32 位**加载器**是否存在：
#
#     if [ -f /lib/ld-linux.so.2 ] || [ -f /lib32/ld-linux.so.2 ]; then
#         echo "  已具备 32 位加载器"      # <- 然后就跳过了整块安装
#     else
#         ... 逐个 apt-get install ...
#     fi
#
# 这个条件是错的，而且错得很隐蔽：libc6:i386 与 libstdc++6:i386 是**两个独立的包**，
# 前者存在不代表后者存在。快照/克隆出来的机器上常常只有 libc6:i386，
# 于是脚本高高兴兴报"已具备"，而 libstdc++6:i386 从来没装上 ——
# 直到 pack 阶段才炸：
#
#     dragon: error while loading shared libraries: libstdc++.so.6
#     cannot open shared object file: No such file or directory
#
# 修法：**逐个库检查**，缺哪个装哪个，最后再整体复核一遍。
#
# 各库与症状的对应关系（都实际踩过）：
#   libc6:i386      提供 ld-linux.so.2。缺它 dragon 报
#                   "No such file or directory" —— 文件明明在，极具误导性
#   libstdc++6:i386 dragon 是 C++ 写的。缺它报 libstdc++.so.6 找不到
#   libgcc-s1:i386  libstdc++ 的运行时依赖
#   lib32z1         覆盖常见的 libz 依赖

# 先启用 i386 架构（幂等）
dpkg --add-architecture i386 >/dev/null 2>&1
apt-get update -qq >/dev/null 2>&1

# 每项：包名|用于探测的路径|缺失时的症状
LIBS=(
    "libc6:i386|/lib/ld-linux.so.2|dragon 报 No such file or directory（文件其实在）"
    "libstdc++6:i386|/usr/lib/i386-linux-gnu/libstdc++.so.6|dragon 报 libstdc++.so.6 找不到"
    "libgcc-s1:i386|/usr/lib/i386-linux-gnu/libgcc_s.so.1|libstdc++ 运行时依赖缺失"
    "lib32z1|/usr/lib32/libz.so.1|libz 依赖缺失"
)

MISSING=0
for entry in "${LIBS[@]}"; do
    pkg="${entry%%|*}"
    rest="${entry#*|}"
    probe="${rest%%|*}"
    symptom="${rest##*|}"

    if [ -e "$probe" ]; then
        echo "  OK       $pkg  ($probe)"
        continue
    fi

    echo "  缺失     $pkg  —— 不装会表现为：$symptom"
    if apt-get install -y "$pkg" >/dev/null 2>&1 && [ -e "$probe" ]; then
        echo "  已装上   $pkg"
    else
        # 探测路径因发行版而异，包状态也算通过
        if dpkg -l "$pkg" 2>/dev/null | grep -q '^ii'; then
            echo "  已装上   $pkg（探测路径不一致，但包状态为已安装）"
        else
            echo "  [!] $pkg 安装失败"
            MISSING=1
            RC=1
        fi
    fi
done

echo
if (( MISSING == 0 )); then
    echo "  全部 32 位库就绪。dragon 可执行性自检："
    DRAGON="$SRC_ROOT_LICHEE/tools/tool/dragon"
    if [ -x "$DRAGON" ]; then
        if "$DRAGON" >/dev/null 2>&1; then
            echo "    ✓ dragon 可执行"
        else
            # dragon 不带参数会打印用法并返回非零，这属于正常
            echo "    ✓ dragon 可加载（打印用法即算通过）"
        fi
    else
        echo "    (dragon 尚未生成，跳过自检)"
    fi
else
    echo "  [!] 仍有 32 位库缺失，打包会失败（但不影响已生成的 nuttx / vela.bin）"
fi

echo
if (( RC == 0 )); then
    echo "全部就绪，可以执行打包。"
else
    echo "有项目未就绪（不会影响已生成的 nuttx / vela.bin）。"
fi
exit $RC
