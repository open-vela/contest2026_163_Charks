#!/usr/bin/env bash
# ============================================================================
# ECS 引导：把一台干净的 Ubuntu 22.04 变成"能编 openvela 的机器"
#
# 为什么单独拆出来
# ----------------
# 之前的 `build_openvela.sh` 把「装依赖 + 同步源码 + 编译 + 打包」串在一条命令里。
# 但前三步要 1 小时以上、而且**只需要做一次**；后两步每次改代码都要重跑。
# 拆开之后：
#     ecs_bootstrap.sh   ← 一次性，可以 nohup 挂着跑
#     build_openvela.sh  ← 每次改动，只跑编译与打包
#
# 幂等：已装的包跳过、已 init 的 .repo 跳过、sync 断点续传。
#
# 用法（在 ECS 上）：
#     nohup bash scripts/ecs_bootstrap.sh > /root/bootstrap.log 2>&1 &
#     tail -f /root/bootstrap.log
# ============================================================================

set -uo pipefail

SRC_ROOT="${OPENVELA_SRC:-/root/openvela}"
MANIFEST_URL="${MANIFEST_URL:-https://gitee.com/open-vela/manifests.git}"
MANIFEST_BRANCH="${MANIFEST_BRANCH:-dev-ai-contest-2026}"
MANIFEST_FILE="${MANIFEST_FILE:-openvela.xml}"
LOG="/root/bootstrap.log"

step() { echo; echo "=== $* ==="; }
ok()   { echo "  [OK]   $*"; }
warn() { echo "  [WARN] $*"; }
die()  { echo "  [ERR]  $*" >&2; exit 1; }

echo "=================================================================="
echo " ECS 引导 @ $(date -Is)"
echo "=================================================================="
echo "源码目录 : $SRC_ROOT"
echo "manifest : $MANIFEST_URL ($MANIFEST_BRANCH / $MANIFEST_FILE)"
echo "内存     : $(awk '/MemTotal/{printf "%.1fG", $2/1048576}' /proc/meminfo)"
echo "可用磁盘 : $(df -BG --output=avail "$SRC_ROOT" 2>/dev/null | tail -1 | tr -d ' G' || df -BG --output=avail / | tail -1 | tr -d ' G')G"
echo "CPU      : $(nproc) 核"

# ---------------------------------------------------------------- 0. swap
step "0/5 交换空间"
# 编译期链接阶段（LTO）瞬时内存需求很高，4 核 16G 也要 swap 兜底。
if [[ "$(free -g | awk '/Swap:/{print $2}')" -lt 4 ]]; then
    if [[ ! -f /swapfile ]]; then
        fallocate -l 12G /swapfile || dd if=/dev/zero of=/swapfile bs=1M count=12288
        chmod 600 /swapfile
        mkswap /swapfile >/dev/null
    fi
    swapon /swapfile 2>/dev/null || true
    grep -q '^/swapfile' /etc/fstab || echo '/swapfile none swap sw 0 0' >> /etc/fstab
    ok "swap 就绪：$(free -g | awk '/Swap:/{print $2}')G"
else
    ok "swap 已有 $(free -g | awk '/Swap:/{print $2}')G"
fi

# ---------------------------------------------------------------- 1. 依赖
step "1/5 安装构建依赖"
export DEBIAN_FRONTEND=noninteractive
apt-get update -y >/dev/null 2>&1 || warn "apt-get update 有报错，继续"

DEPS=(
    build-essential git git-lfs cmake ninja-build ccache pkg-config
    autoconf automake libtool libtool-bin m4
    bison flex gperf texinfo help2man gawk bc rsync
    libncurses-dev libncursesw5-dev libssl-dev zlib1g-dev
    device-tree-compiler libusb-1.0-0-dev
    python3 python3-pip python3-venv python3-setuptools python3-yaml
    wget curl unzip zip xz-utils file
    repo
    # dragon（打包工具）是 i386 程序，缺 32 位运行库会报
    # "No such file or directory"（而文件其实就在那里）
    libc6-i386 lib32z1 lib32ncurses6 lib32stdc++6
)
apt-get install -y "${DEPS[@]}" >/dev/null 2>&1 || {
    warn "批量安装有失败项，逐个重试"
    for p in "${DEPS[@]}"; do
        dpkg -s "$p" >/dev/null 2>&1 || apt-get install -y "$p" >/dev/null 2>&1 \
            || warn "装不上：$p"
    done
}

MISSING=""
for t in git git-lfs repo make gcc python3; do
    command -v "$t" >/dev/null 2>&1 && ok "$t" || MISSING="$MISSING $t"
done
[[ -z "$MISSING" ]] || die "缺工具：$MISSING"

# ---------------------------------------------------------------- 2. git
step "2/5 配置 git / git-lfs"
git config --global http.postBuffer 524288000
git config --global http.lowSpeedLimit 0
git config --global http.lowSpeedTime 999999
git config --global core.compression 0
git config --global user.name  >/dev/null 2>&1 || git config --global user.name  "openvela-builder"
git config --global user.email >/dev/null 2>&1 || git config --global user.email "builder@localhost"
git lfs install >/dev/null 2>&1 && ok "git-lfs 全局过滤器已启用" || warn "git-lfs 初始化失败"

# apt 的 repo 是 2.17，不支持 --git-lfs 等新参数。清华镜像在云主机上可达
# （家用网络曾 403，所以这里失败也不致命）。
if ! repo --version 2>&1 | grep -qE "(2\.[2-9][0-9]|[3-9]\.)"; then
    if git clone --depth 1 https://mirrors.tuna.tsinghua.edu.cn/git/git-repo/ \
           /opt/git-repo >/dev/null 2>&1; then
        chmod +x /opt/git-repo/repo
        ln -sf /opt/git-repo/repo /usr/local/bin/repo
        hash -r 2>/dev/null || true
        ok "升级到新版 repo：$(repo --version 2>&1 | head -1)"
    else
        warn "新版 repo 装不上，继续用 apt 版"
    fi
fi

mkdir -p "$SRC_ROOT"

# ---------------------------------------------------------------- 3. init
step "3/5 repo init"
cd "$SRC_ROOT"
if [[ -d .repo ]]; then
    ok "已存在 .repo，跳过 init"
else
    REPO_LFS_FLAG=()
    repo init --help 2>&1 | grep -q -- "--git-lfs" && REPO_LFS_FLAG=(--git-lfs)

    ok_init=0
    for mirror in "" "https://mirrors.tuna.tsinghua.edu.cn/git/git-repo/"; do
        extra=()
        [[ -n "$mirror" ]] && extra=(--repo-url="$mirror")
        if repo init -u "$MANIFEST_URL" -b "$MANIFEST_BRANCH" -m "$MANIFEST_FILE" \
                "${REPO_LFS_FLAG[@]}" "${extra[@]}" --no-repo-verify 2>&1 | tail -20; then
            ok_init=1; break
        fi
        warn "该方式失败，换下一种"
    done
    [[ "$ok_init" == "1" ]] || die "repo init 失败。可试 GitCode 源：
        rm -rf $SRC_ROOT/.repo && repo init -u https://gitcode.com/open-vela/manifests.git \
            -b $MANIFEST_BRANCH -m $MANIFEST_FILE --git-lfs"
    ok "repo init 完成"
fi

# ---------------------------------------------------------------- 4. sync
step "4/5 repo sync（最慢的一步，断线直接重跑本脚本续传）"
echo "  开始 $(date -Is)"
repo sync -c --no-tags -j8 --force-sync --no-clone-bundle 2>&1 | tail -30
# repo sync 的退出码在部分版本上不可靠，用"关键目录在不在"来判定
if [[ -d "$SRC_ROOT/vendor/allwinnertech/boards/r528/r528s3-gemini-s1" \
   && -d "$SRC_ROOT/packages/ai_agent" \
   && -f "$SRC_ROOT/build.sh" ]]; then
    ok "源码同步完成 $(date -Is)"
else
    die "同步不完整：关键目录缺失。直接重跑本脚本续传即可。"
fi

# ---------------------------------------------------------------- 5. 校验
step "5/5 校验"
cd "$SRC_ROOT"
du -sh "$SRC_ROOT" 2>/dev/null | sed 's/^/  /'
echo "  板级配置："
ls -1 vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/ 2>/dev/null | sed 's/^/    /'
echo "  ai_agent："
ls -1 packages/ai_agent/ 2>/dev/null | head -8 | sed 's/^/    /'

echo
echo "=================================================================="
echo " 引导完成 @ $(date -Is)"
echo " 下一步（编译 + 打包）："
echo "   bash /root/build/scripts/build_openvela.sh"
echo "=================================================================="
