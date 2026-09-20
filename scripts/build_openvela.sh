#!/usr/bin/env bash
#
# openvela 一键构建（Gemini-S1 / R528）
#
# 用法（在 WSL 的 Ubuntu 22.04 里执行）：
#     bash scripts/build_openvela.sh
#
# 可选：
#     OPENVELA_SRC=~/my-src bash scripts/build_openvela.sh    # 换源码目录
#     JOBS=4 bash scripts/build_openvela.sh                    # 限制并行度
#
# 它按顺序做这些事，每一步都可重复执行（已完成的会跳过）：
#     0. 环境自检（Ubuntu 版本 / 磁盘 / 内存 / 是否误放在 /mnt/c）
#     1. 安装构建依赖
#     2. 配置 git 与 git-lfs
#     3. repo init —— 从 Gitee 拉 manifest（国内速度）
#     4. repo sync —— 同步全部源码
#     5. 确认 Gemini-S1 板级配置存在
#     6. 编译固件
#     7. 打包成可烧录镜像
#
# 为什么从 Gitee 而不是 GitHub：
#   openvela.xml 里的 remote 用的是**相对地址**（fetch="../open-vela/"），
#   没有任何绝对 URL。也就是说 repo 会从"取 manifest 的那个站"拉全部仓库。
#   从 Gitee 起步 → 全程走 Gitee，国内速度可用；从 GitHub 起步在国内会非常慢。
#   已实测：Gitee 上 manifests/nuttx-apps/vendor_allwinnertech/packages_ai_agent
#   四个仓库都在，dev-ai-contest-2026 分支存在，且匿名克隆可用（无需登录）。

set -euo pipefail

# ---------------------------------------------------------------------------
# 配置
# ---------------------------------------------------------------------------

MANIFEST_URL="https://gitee.com/open-vela/manifests.git"
MANIFEST_BRANCH="dev-ai-contest-2026"
MANIFEST_FILE="openvela.xml"

BOARD_PATH="vendor/allwinnertech/boards/r528/r528s3-gemini-s1"

# 板级配置，可用环境变量覆盖：
#   BOARD_CONFIG=configs/nsh_minidisplay bash scripts/build_openvela.sh
#
# 两者差别（实测对比 defconfig 得出）：
#   configs/nsh             纯命令行，无屏幕
#   configs/nsh_minidisplay 带 2.8 寸 ILI9341 SPI 屏 + LVGL + 图形启动器
#                           （CONFIG_LUNCHER_MINI_APP=y），即出厂固件那套界面；
#                           另开了 CONFIG_LTO_FULL=y，链接阶段会明显更慢
BOARD_CONFIG="${BOARD_CONFIG:-configs/nsh}"

SRC_ROOT="${OPENVELA_SRC:-$HOME/openvela}"
LOG_FILE="$HOME/openvela-build.log"

# 本脚本所在目录（用 BASH_SOURCE 而不是 $0 —— $0 在某些 shell 下不可靠）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 编译并行度：默认按内存自动推算，避免 15GB 机器上 -j16 把内存打爆
if [[ -z "${JOBS:-}" ]]; then
    MEM_GB=$(awk '/MemTotal/ {printf "%d", $2/1024/1024}' /proc/meminfo)
    if   (( MEM_GB < 6  )); then JOBS=2
    elif (( MEM_GB < 10 )); then JOBS=4
    elif (( MEM_GB < 20 )); then JOBS=6
    else                         JOBS=8
    fi
fi

# ---------------------------------------------------------------------------
# 输出helpers
# ---------------------------------------------------------------------------

if [[ -t 1 ]]; then
    C_RED=$'\033[31m'; C_GRN=$'\033[32m'; C_YEL=$'\033[33m'
    C_CYA=$'\033[36m'; C_BLD=$'\033[1m';  C_RST=$'\033[0m'
else
    C_RED=""; C_GRN=""; C_YEL=""; C_CYA=""; C_BLD=""; C_RST=""
fi

step()  { echo; echo "${C_CYA}==============================================================${C_RST}"; \
          echo "${C_BLD} $*${C_RST}"; \
          echo "${C_CYA}==============================================================${C_RST}"; }
ok()    { echo "  ${C_GRN}✓${C_RST} $*"; }
warn()  { echo "  ${C_YEL}!${C_RST} $*"; }
err()   { echo "  ${C_RED}✗${C_RST} $*" >&2; }
die()   { err "$*"; echo; err "构建中止。完整日志：$LOG_FILE"; exit 1; }

# ---------------------------------------------------------------------------
# 第 0 步：环境自检
# ---------------------------------------------------------------------------

step "第 0 步 / 环境自检"

# ---------------------------------------------------------------------------
# 先跑官方检测脚本
#
# 它由 openvela 官方维护（open-vela/.claude 仓库的 skills/openvela-quickstart），
# 判定标准权威，而且只要 2 秒。放在最前面是为了在付出几十分钟同步之前，
# 就拿到"这个环境到底能不能编译"的明确答复，而不是编到一半才失败。
#
# 已知它的判定规则（原文）：
#   运行环境：grep -qi microsoft /proc/version → "WSL 环境 (不支持编译)"
#             [ -f /.dockerenv ]              → "Docker 环境 (不支持编译)"
#             否则                             → "原生 Linux"
#   操作系统：Ubuntu 22.04 → 通过；其他 Ubuntu → 警告；非 Ubuntu → 失败
#   内存    ：≥15360MB 通过；≥7680MB 警告；更低失败
#   磁盘    ：≥40GB 通过；≥20GB 警告；更低失败
#
# 注意它**无法区分虚拟机和裸机** —— VMware 里的真 Ubuntu 不含 microsoft 标记，
# 会被判为「原生 Linux」并通过。这也是润芯微的 VMware 教程可行的原因。
# ---------------------------------------------------------------------------

# 兼容两种摆放方式（远端上传时目录层级可能与本地不同）：
#   <scripts>/official/detect-env.sh      本地仓库的结构
#   <scripts>/../official/detect-env.sh   官方脚本与 scripts 平级
OFFICIAL_DETECT=""
for cand in "$SCRIPT_DIR/official/detect-env.sh" "$SCRIPT_DIR/../official/detect-env.sh"; do
    if [[ -f "$cand" ]]; then
        OFFICIAL_DETECT="$cand"
        break
    fi
done

if [[ -n "$OFFICIAL_DETECT" ]]; then
    echo "  运行官方环境检测（$OFFICIAL_DETECT）..."
    echo
    if ! bash "$OFFICIAL_DETECT"; then
        echo
        warn "官方检测未通过 —— 见上面的 ✗ 项。"
        warn "其中「WSL 环境 (不支持编译)」是官方明确的能力边界，不是误报。"
        echo
        if [[ -t 0 ]]; then
            read -r -p "  仍要继续吗？(y/N) " ans
            [[ "$ans" == "y" || "$ans" == "Y" ]] || { echo "  已中止。"; exit 1; }
        else
            warn "非交互环境，继续执行。请自行判断风险。"
        fi
    else
        ok "官方检测通过"
    fi
    echo
else
    warn "未找到 $OFFICIAL_DETECT，跳过官方检测（继续用本脚本自己的检查）"
fi

if [[ ! -f /etc/lsb-release ]]; then
    die "这不是 Ubuntu（或文件系统异常）。openvela 要求 64 位 Ubuntu 22.04。"
fi

# shellcheck disable=SC1091
. /etc/lsb-release
echo "  发行版    : ${DISTRIB_DESCRIPTION:-未知}"

if [[ "${DISTRIB_RELEASE:-}" != "22.04" ]]; then
    warn "官方要求 Ubuntu 22.04，当前是 ${DISTRIB_RELEASE:-未知}。"
    warn "其他版本通常也能编译，但遇到工具链问题时请优先怀疑版本差异。"
fi

ARCH=$(uname -m)
echo "  架构      : $ARCH"
[[ "$ARCH" == "x86_64" ]] || die "openvela 要求 x86_64 主机，当前是 $ARCH。"

# 源码不能放在 /mnt/c —— 跨 9P 文件系统会导致编译极慢，且符号链接/权限会出问题
if [[ "$SRC_ROOT" == /mnt/* ]]; then
    die "源码目录不能放在 $SRC_ROOT（即 Windows 盘）。
       跨文件系统编译会非常慢，且 openvela 的部分构建步骤依赖 Linux 原生的
       文件权限与符号链接，放在 /mnt/c 下会失败。
       请用默认位置：$HOME/openvela"
fi

mkdir -p "$SRC_ROOT"

MEM_GB=$(awk '/MemTotal/ {printf "%.1f", $2/1024/1024}' /proc/meminfo)
DISK_GB=$(df -BG --output=avail "$SRC_ROOT" | tail -1 | tr -d ' G')
SWAP_GB=$(free -g | awk '/Swap:/ {print $2}')

echo "  内存      : ${MEM_GB} GB"
echo "  交换空间  : ${SWAP_GB} GB"
echo "  可用磁盘  : ${DISK_GB} GB"
echo "  源码目录  : $SRC_ROOT"
echo "  并行度    : -j${JOBS}（可用 JOBS=N 覆盖）"

# 门槛对齐官方 detect-env.sh 的判定（≥40GB 通过）。
# 早先这里写的是 80GB —— 那是我自己的保守估计，会把官方判定合格、
# 而系统盘只有 40-60GB 的常见云主机全部误杀。以官方标准为准。
#
# 但要注意：40GB 是"全新拉取源码 + 完整编译"的量级。若源码已经同步好、
# 只是断点续编，剩余需求小得多（源码本身已占 31GB）。
# 所以允许用 MIN_DISK_GB 覆盖，续跑时可以调低：
#     MIN_DISK_GB=8 bash scripts/build_openvela.sh
MIN_DISK_GB="${MIN_DISK_GB:-40}"

if (( DISK_GB < MIN_DISK_GB )); then
    die "可用磁盘只有 ${DISK_GB}GB，低于门槛 ${MIN_DISK_GB}GB。
       官方判定规则：≥40GB 通过，≥20GB 警告，低于 20GB 失败。
       若源码已同步好只想续编，可临时调低：MIN_DISK_GB=8 bash $0"
elif (( DISK_GB < 60 )); then
    warn "可用磁盘 ${DISK_GB}GB（达到 40GB 最低线，但建议 60GB 以上）。"
    warn "源码同步 + 编译产物可能比较吃紧，构建过程中留意磁盘占用。"
fi

if awk "BEGIN{exit !($MEM_GB < 12)}"; then
    warn "内存只有 ${MEM_GB}GB，编译可能因 OOM 失败。"
fi

if (( SWAP_GB < 8 )); then
    warn "交换空间只有 ${SWAP_GB}GB，建议加到 16GB 以减少 OOM 风险。"
    warn "在 Windows 侧创建 %USERPROFILE%\\.wslconfig，内容："
    warn "    [wsl2]"
    warn "    memory=12GB"
    warn "    swap=16GB"
    warn "    processors=12"
    warn "然后执行 wsl --shutdown 让其生效（会重启 WSL）。"
fi

ok "环境自检通过"

# ---------------------------------------------------------------------------
# 第 1 步：安装依赖
# ---------------------------------------------------------------------------

step "第 1 步 / 安装构建依赖"

if [[ ! -f /var/lib/apt/lists/lock ]] || ! dpkg -s build-essential >/dev/null 2>&1; then
    echo "  更新软件源索引..."
    sudo apt-get update -y || warn "apt-get update 有报错，继续尝试安装"
else
    ok "依赖似乎已装过，仍会补装缺失项"
fi

DEPS=(
    # 基础编译工具
    build-essential git git-lfs cmake ninja-build ccache pkg-config
    autoconf automake libtool libtool-bin m4
    # openvela / NuttX 构建常用
    bison flex gperf texinfo help2man gawk bc rsync
    libncurses-dev libncursesw5-dev libssl-dev zlib1g-dev
    device-tree-compiler libusb-1.0-0-dev
    # 脚本与下载
    python3 python3-pip python3-venv python3-setuptools python3-yaml
    wget curl unzip zip xz-utils file
    # repo 工具（Ubuntu 自带包，避免从 storage.googleapis.com 下载——
    # 那个地址在国内通常打不开）
    repo
)

# 一次性安装；失败的包单独重试，避免因为一个包不可用就整体中止
if ! sudo apt-get install -y "${DEPS[@]}" 2>&1 | tee -a "$LOG_FILE"; then
    warn "批量安装有失败项，逐个重试..."
    for pkg in "${DEPS[@]}"; do
        dpkg -s "$pkg" >/dev/null 2>&1 || {
            sudo apt-get install -y "$pkg" >>"$LOG_FILE" 2>&1 || warn "装不上：$pkg"
        }
    done
fi

for tool in git git-lfs repo make python3; do
    if command -v "$tool" >/dev/null 2>&1; then
        ok "$tool 可用"
    else
        die "$tool 不可用。请手动安装后重跑：sudo apt-get install -y $tool"
    fi
done

# ---------------------------------------------------------------------------
# 第 2 步：配置 git
# ---------------------------------------------------------------------------

step "第 2 步 / 配置 git"

# repo 同步大量仓库时，默认的 http.postBuffer 会在推大包时报错
git config --global http.postBuffer 524288000 || true
git config --global http.lowSpeedLimit 0        || true
git config --global http.lowSpeedTime 999999    || true
git config --global core.compression 0          || true

# 提交时需要 user.name/email，repo 同步过程中会用到
if ! git config --global user.name >/dev/null 2>&1; then
    git config --global user.name  "openvela-builder"
    git config --global user.email "builder@localhost"
    ok "已设置占位 git 身份（仅本机使用）"
fi

if git lfs install >/dev/null 2>&1; then
    ok "git-lfs 已初始化（全局过滤器已启用，repo 走普通 clone 即可拉到 LFS 实体）"
else
    warn "git-lfs 初始化失败；若同步时大量 LFS 文件报错，先修好它再继续"
fi

# 尝试用较新的 repo（apt 自带的 2.17 偏旧，不支持 --git-lfs 等新参数）。
# 已实测：从云主机访问清华镜像是通的（而从家用网络是 403，网络环境不同）。
# 装新版失败不影响继续——旧版只是少几个可选参数。
if ! repo --version 2>&1 | grep -qE "(2\.[2-9][0-9]|[3-9]\.)"; then
    echo "  尝试安装较新的 repo（清华镜像）..."
    if git clone --depth 1 https://mirrors.tuna.tsinghua.edu.cn/git/git-repo/ \
           /opt/git-repo >>"$LOG_FILE" 2>&1; then
        chmod +x /opt/git-repo/repo
        ln -sf /opt/git-repo/repo /usr/local/bin/repo
        hash -r 2>/dev/null || true
        ok "已安装新版 repo：$(repo --version 2>&1 | head -1)"
    else
        warn "装新版 repo 失败，继续用系统自带的版本"
    fi
fi

# repo 需要 Python 3
if ! repo --version >/dev/null 2>&1; then
    warn "repo --version 输出异常，但命令存在，继续尝试"
else
    ok "repo 可用"
fi

# ---------------------------------------------------------------------------
# 第 3 步：repo init
# ---------------------------------------------------------------------------

step "第 3 步 / repo init（从 Gitee 拉 manifest）"

cd "$SRC_ROOT"

if [[ -d .repo ]]; then
    ok "已存在 .repo，跳过 init（若要换分支请先删除 $SRC_ROOT/.repo）"
else
    echo "  manifest : $MANIFEST_URL"
    echo "  分支     : $MANIFEST_BRANCH"
    echo "  清单     : $MANIFEST_FILE"
    echo

    # --git-lfs 是官方 init-repo.sh 用的参数，但**不是所有 repo 版本都支持**：
    # Ubuntu 22.04 自带的 repo 是 2.17，会直接报
    #     repo: error: no such option: --git-lfs
    # 而且这个错误发生在参数解析阶段，会让人误以为是镜像不通。
    # 实测踩过：两次尝试都是被这个参数挡掉的，镜像根本没被访问到。
    #
    # 所以按版本探测。旧版 repo 不带这个参数也能拿到 LFS 文件——
    # 因为本脚本已经执行过 `git lfs install`，它写入全局 filter.lfs.* 配置，
    # repo 内部走的是普通 git clone，smudge 过滤器会自动拉取 LFS 内容。
    REPO_LFS_FLAG=()
    if repo init --help 2>&1 | grep -q -- "--git-lfs"; then
        REPO_LFS_FLAG=(--git-lfs)
        ok "当前 repo 支持 --git-lfs，已启用"
    else
        warn "当前 repo 不支持 --git-lfs（版本较旧），改用 git lfs install 的全局过滤器"
        warn "效果等价：普通 git clone 会自动拉取 LFS 实体内容"
    fi

    REPO_INIT_BASE=(
        -u "$MANIFEST_URL"
        -b "$MANIFEST_BRANCH"
        -m "$MANIFEST_FILE"
        "${REPO_LFS_FLAG[@]}"
    )

    # repo init 除了拉 manifest，还会去下载 repo 工具自身的源码。
    # 默认地址是 gerrit.googlesource.com，国内打不开。
    # 官方给 Gitee/GitCode 指定了清华镜像，但本机实测该镜像返回 403，
    # 所以这里做成级联回退，而不是写死一个：
    #   ① 先不带 --repo-url —— Ubuntu 的 apt repo 包通常自带源码，走本地
    #   ② 失败则试清华镜像（官方指定的那个）
    #   ③ 再失败就明确报错并给出人工排查方向
    REPO_MIRRORS=(
        ""                                              # ① 用本地/默认
        "https://mirrors.tuna.tsinghua.edu.cn/git/git-repo/"  # ② 官方指定镜像
    )

    init_ok=0
    for mirror in "${REPO_MIRRORS[@]}"; do
        extra=()
        if [[ -n "$mirror" ]]; then
            extra=(--repo-url="$mirror")
            echo "  尝试 repo 工具镜像: $mirror"
        else
            echo "  尝试使用本地 repo（apt 安装的）"
        fi

        if repo init "${REPO_INIT_BASE[@]}" "${extra[@]}" --no-repo-verify 2>&1 | tee -a "$LOG_FILE"; then
            init_ok=1
            break
        fi
        warn "该方式失败，换下一种"
    done

    if [[ "$init_ok" != "1" ]]; then
        cat >&2 <<EOF

  ✗ repo init 全部方式都失败。

  最可能的原因：repo 工具自身的源码拉不下来（默认地址 gerrit.googlesource.com
  在国内打不开，而清华镜像此刻也返回 403）。

  可行的绕法（按顺序试）：
    1. 确认 repo 是 apt 装的：  sudo apt-get install -y repo
       然后清掉半成品再重跑本脚本：
         rm -rf $SRC_ROOT/.repo
    2. 手动指定其他镜像（找一个你能 ping 通的）：
         repo init -u $MANIFEST_URL -b $MANIFEST_BRANCH -m $MANIFEST_FILE \\
                   --repo-url=<可用镜像> --git-lfs
    3. 换成 GitCode 源（已实测该站有 dev-ai-contest-2026 分支）：
         rm -rf $SRC_ROOT/.repo
         repo init -u https://gitcode.com/open-vela/manifests.git \\
                   -b $MANIFEST_BRANCH -m $MANIFEST_FILE --git-lfs

EOF
        die "repo init 失败"
    fi

    ok "repo init 完成"
fi

# ---------------------------------------------------------------------------
# 第 4 步：repo sync
# ---------------------------------------------------------------------------

step "第 4 步 / repo sync（同步源码，这一步最慢）"

echo "  仓库很多，首次同步视网络需要 20-60 分钟甚至更久。"
echo "  中途断了不要紧：重跑本脚本会自动续传。"
echo

# 只用最通用的参数，避免因 repo 版本差异失败：
#   -c        只同步当前分支（省流量）
#   --no-tags 不拉标签
# 断点续传是 repo 自带的，重跑本脚本即可。
repo sync -c --no-tags -j8 2>&1 | tee -a "$LOG_FILE" \
    || die "repo sync 失败。多为网络中断——直接重跑本脚本即可续传。"

ok "源码同步完成"

# ---------------------------------------------------------------------------
# 第 5 步：确认板级配置
# ---------------------------------------------------------------------------

step "第 5 步 / 确认 Gemini-S1 板级配置"

cd "$SRC_ROOT"

[[ -d "$BOARD_PATH" ]] \
    || die "找不到板级目录 $BOARD_PATH。
       可能原因：manifest 分支不对（应为 $MANIFEST_BRANCH）。
       可以先看看实际有哪些板子：ls vendor/allwinnertech/boards/r528/"

echo "  板级目录内容："
ls -1 "$BOARD_PATH" | sed 's/^/    /'

CONFIG_DIR="$BOARD_PATH/$BOARD_CONFIG"
[[ -d "$CONFIG_DIR" ]] \
    || die "找不到配置目录 $CONFIG_DIR。
       可用配置：ls $BOARD_PATH/configs/"

echo
echo "  使用的配置：$BOARD_CONFIG"
ls -1 "$CONFIG_DIR" | sed 's/^/    /'

[[ -f "$SRC_ROOT/build.sh" ]] || die "找不到 $SRC_ROOT/build.sh，源码树不完整。"

ok "板级配置就绪"

# ---------------------------------------------------------------------------
# 第 5.5 步：源码修补
# ---------------------------------------------------------------------------
#
# openvela 上游与厂商配置里有几处**必须修补才能编过**的地方。它们不是业务代码，
# 而是构建链适配，所以集中在这里、每次构建前自动应用（全部幂等）。
# 每个补丁的现象/诊断/根因见 scripts/云构建实录.md。

step "第 5.5 步 / 应用源码修补"

# 复用文件开头就算好的 $SCRIPT_DIR，**不要**在这里重新从 $0 推导。
#
# 踩过的坑：这里原本写的是
#     PATCHER_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
# 而调用方（remote_build_job.sh）是先 `cd $BUILD_DIR` 再 `bash scripts/build_openvela.sh`，
# 于是 $0 是**相对路径** `scripts/build_openvela.sh`。脚本跑到这里时 cwd 早已被
# 前面的步骤改成了 $SRC_ROOT，readlink 就从错误的 cwd 去解析，得到了
# /root/openvela 这种错目录。
#
# 症状很具误导性：日志说"找不到 fix_vendor_make_defs.py""找不到 swscale_shim.c"，
# 看起来像文件没上传，实际是路径算错了 —— 而且补丁① 会**静默跳过**（只 warn），
# 不报错、不中断，更难发现。
PATCHER_DIR="$SCRIPT_DIR"
BOARD_SRC="$SRC_ROOT/vendor/allwinnertech/boards/r528/r528s3-gemini-s1/src"

# ① QuickApp 预编译库的 whole-archive 写法必须跟随实际链接器。
#    nsh 用裸 ld、nsh_minidisplay 因 LTO 换成 gcc，两者要求相反的前缀。
if [[ -f "$PATCHER_DIR/fix_vendor_make_defs.py" ]]; then
    python3 "$PATCHER_DIR/fix_vendor_make_defs.py" 2>&1 | tee -a "$LOG_FILE" || true
else
    warn "找不到 fix_vendor_make_defs.py，跳过补丁 ①"
fi

# ② libswscale 缺失符号的 shim（详见云构建实录坑 6）
#
# 必须**自己复制**，不能只检查：下面的第 ③ 步会无条件把 swscale_shim.c 注册进
# CSRCS，若文件不在位，make 会直接报
#     No rule to make target 'swscale_shim.c', needed by '.depend'
# —— 而且这个报错发生在 pass2dep 阶段，看起来像构建系统坏了，不像是缺文件。
#
# 上一轮是手工把 shim 传上去的，所以这个洞没暴露；换到全新源码树就跑不过。
# 补上复制逻辑后，脚本才真正自包含。
SHIM_SRC="$PATCHER_DIR/../board/patches/swscale_shim.c"
if [[ -f "$BOARD_SRC/swscale_shim.c" ]]; then
    ok "swscale_shim.c 已在位"
elif [[ -f "$SHIM_SRC" ]]; then
    cp "$SHIM_SRC" "$BOARD_SRC/swscale_shim.c"
    ok "swscale_shim.c 已从 board/patches/ 复制到位"
else
    die "找不到 swscale_shim.c（既不在 $BOARD_SRC，也不在 $SHIM_SRC）。
       QuickApp 预编译库会引用 libswscale 的 sws_* 符号，缺这个 shim 会链接失败。
       确认 board/patches/swscale_shim.c 已随本脚本一起上传到构建套件目录。"
fi

# ③ 确认 shim 已注册进 CSRCS。文件在但没注册 = 没编进去，症状一样是"未定义符号"，
#    排查时容易白找一圈。
if grep -q swscale_shim "$BOARD_SRC/Makefile" 2>/dev/null; then
    ok "swscale_shim.c 已注册到 CSRCS"
else
    warn "CSRCS 里没有 swscale_shim.c，自动补上"
    sed -i 's|shim_binary_compat.c|shim_binary_compat.c swscale_shim.c|' \
        "$BOARD_SRC/Makefile" 2>/dev/null || true
fi

# ④ Git LFS 托管的预编译库。指针文件会导致链接报
#    "file format not recognized; treating as linker script"。
#    这个脚本自身会检测"已是真实库则跳过"，所以每次跑代价很低。
if [[ -f "$PATCHER_DIR/fetch_lfs_libs.py" ]]; then
    python3 "$PATCHER_DIR/fetch_lfs_libs.py" 2>&1 | tail -6 | tee -a "$LOG_FILE" || true
fi

# ---------------------------------------------------------------------------
# 第 6 步：编译
# ---------------------------------------------------------------------------

step "第 6 步 / 编译固件"

echo "  配置 : $CONFIG_DIR"
echo "  并行 : -j${JOBS}"
echo

BUILD_START=$(date +%s)

# 切换板级配置时必须全量重建：两个配置的 .config 不同，若沿用上一次的目标
# 文件，会得到难以理解的链接错误（缺符号 / 重复定义），而且报错位置离真因很远。
# 这里用标记文件记住上次编的是哪个配置，检测到变化就自动升级为全量重建。
STAMP="$HOME/openvela-build/.last_config"
mkdir -p "$(dirname "$STAMP")"
LAST_CFG="$(cat "$STAMP" 2>/dev/null || true)"

if [[ -n "$LAST_CFG" && "$LAST_CFG" != "$BOARD_CONFIG" ]]; then
    echo "  检测到配置切换：$LAST_CFG → $BOARD_CONFIG"
    echo "  自动执行 distclean 全量重建（沿用旧目标文件会导致链接错误）"
    FULL_REBUILD=1
fi

# distclean 只在改过 menuconfig 或切换配置时才需要；默认只做增量编译，
# 以免每次重跑都全量重编（那要几十分钟）。
if [[ "${FULL_REBUILD:-0}" == "1" ]]; then
    echo "  FULL_REBUILD=1，先执行 distclean 全量重建"
    ./build.sh "$CONFIG_DIR/" -j"$JOBS" distclean 2>&1 | tee -a "$LOG_FILE" || true
fi

# ---------------------------------------------------------------------------
# 编译参数
# ---------------------------------------------------------------------------
#
# -e         交给 nuttx/tools/configure.sh：**配置变了就自动 distclean**。
#            它只在 defconfig 真的变化时才清，平时不额外花时间；
#            而一旦改了配置（例如 R5 换成带 AI Agent 的 defconfig），
#            它会清掉旧 .config 与目标文件，避免"配置改了但对象文件还是旧的"
#            ——那类错误表现为缺符号/重复定义，报错位置离真因很远。
#            上面基于 .last_config 的检测只能发现"换了配置目录"，
#            发现不了"同一个目录里 defconfig 内容被改了"，所以 -e 是必要的补充。
#
# -Wno-error 不对警告严格。官方给 gemini-s1 的 AI Agent 配置就是这么写的：
#            packages/ai_agent/defconfigs/gemini-s1/README.md 的 Quick Start
#            用的是 `./build.sh <config>/ -e -Wno-error -j$(nproc)`。
#
# 两个都可以用 BUILD_FLAGS 覆盖（例如只想排错时用 BUILD_FLAGS=-e）。
BUILD_FLAGS="${BUILD_FLAGS:--e -Wno-error}"

echo "  额外参数: $BUILD_FLAGS"
echo

# 故意不加引号：这里需要按空格拆成多个参数传给 build.sh。
# shellcheck disable=SC2086
./build.sh "$CONFIG_DIR/" $BUILD_FLAGS -j"$JOBS" 2>&1 | tee -a "$LOG_FILE" \
    || die "编译失败。往上翻日志找第一个 error（不是最后一个）。"

# 编译成功后才记下配置，供下次检测切换
echo "$BOARD_CONFIG" > "$STAMP"

BUILD_SEC=$(( $(date +%s) - BUILD_START ))
ok "编译完成，耗时 $((BUILD_SEC / 60)) 分 $((BUILD_SEC % 60)) 秒"

# ---------------------------------------------------------------------------
# 第 7 步：打包
# ---------------------------------------------------------------------------

step "第 7 步 / 打包成可烧录镜像"

LICHEE_DIR="$SRC_ROOT/vendor/allwinnertech/lichee"

if [[ ! -d "$LICHEE_DIR" ]]; then
    warn "找不到 lichee 目录，跳过打包。产物见上一步输出。"
    exit 0
fi

[[ -f "$LICHEE_DIR/envsetup.sh" ]] || die "找不到 envsetup.sh，lichee 目录结构异常。"

# lichee 这套脚本（envsetup.sh / lunch_nuttx / pack）是按"人工在 bash 里交互
# 敲"写的：会引用未定义的变量（实测 XTENSAD_LICENSE_FILE）、依赖隐含的全局
# 状态。而本脚本开头是 set -euo pipefail，直接 source 会在第一处未定义变量
# 上就中止 —— 这正是打包卡住的原因。
#
# 所以整段放进**子 shell** 并临时关掉严格模式：既让第三方脚本按它原本假设的
# 环境跑，又不让它的副作用（cd、export、函数定义）泄漏回本脚本。
PACK_RC=0

# 这里必须临时关掉 -e：本脚本开了 pipefail，包装的管道一旦非零退出，
# set -e 会让整个脚本立即结束 —— 那样就打不出下面那段有用的提示了。
set +e
(
    set +u
    set +e
    cd "$LICHEE_DIR" || exit 1

    # shellcheck disable=SC1091
    source ./envsetup.sh

    # lunch_nuttx 支持**直接传项目名**（见其实现：有 $1 就不走交互菜单）。
    # 这比喂菜单编号可靠得多 —— 我第一版按 README 猜了编号 "2"，
    # 而实际菜单是 1=dshanpi、2=evb4、3=gemini-s1，于是选错板型，
    # pack 只打印 "platform(%%_*) not support" 就退出了。
    echo "  选择板型：r528s3-gemini-s1 ..."
    lunch_nuttx r528s3-gemini-s1

    echo
    echo "  执行 pack..."
    pack
) 2>&1 | tee -a "$LOG_FILE"
PACK_RC=${PIPESTATUS[0]}
set -e

# 判定打包成败**只看产物，不看返回码**。两次实测都证明返回码不可信，
# 而且两个方向的坑都踩过：
#   - 选错板型：只打印 "platform(%%_*) not support" 就退出，返回码却是 0（假成功）
#   - 打包成功：镜像已生成、日志打印 "pack finish"，返回码却是 1（假失败）
PACK_IMG="$(find "$LICHEE_DIR" -maxdepth 4 -type f -name '*.img' \
            -newermt '-30 minutes' 2>/dev/null | head -1)"

if [[ -n "$PACK_IMG" ]]; then
    ok "打包完成：$PACK_IMG"
    ok "镜像大小：$(( $(stat -c%s "$PACK_IMG") / 1024 / 1024 )) MB"
    warn "（pack 退出码为 $PACK_RC，与结果无关——以产物为准，见脚本注释）"
else
    warn "打包未产出镜像（pack 返回码 $PACK_RC）。"
    warn "固件本身已在第 6 步生成、可直接使用；打包只影响给 PhoenixSuit 的 .img。"
    warn "常见原因：dragon 是 i386 程序，需 32 位运行库。"
    warn "跑一次 bash scripts/install_pack_deps.sh 即可（脚本里记了这个坑）。"
fi

# ---------------------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------------------

step "完成 / 产物位置"

echo "  在 lichee 目录下找最近生成的固件（.img / .bin / .fex 等）："
echo
find "$LICHEE_DIR" -maxdepth 4 -type f \
     \( -name "*.img" -o -name "*.fex" -o -name "*.bin" \) \
     -newermt "-2 hours" -printf "    %10s  %p\n" 2>/dev/null | sort -k2 | tail -20 \
     || echo "    （没找到近期产物，请手动检查 $LICHEE_DIR 下的 out/ 或 tools/ 目录）"

cat <<EOF

  下一步（这部分必须人工，机器替代不了）：
    1. 把上面列出的镜像文件拷到 Windows（可以直接从 \\\\wsl\$ 访问，
       或 cp 到 /mnt/c/Users/<你>/Desktop/）
    2. 用 PhoenixSuit 烧录，过程需要按住板子上的 FEL / RST 键
    3. 烧完用 ADB 连上，验证：

         adb shell "uname -a"
         adb shell "ls /dev/audio"
         adb shell "amixer"           # 看混音器控制项是否齐全

  完整构建日志：$LOG_FILE

EOF
